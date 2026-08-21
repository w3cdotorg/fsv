// src/sdl/gpu.cpp — SPDX-License-Identifier: MIT
//
// SDL_GPU implementation of src/gpu.h for the fsv macOS/Metal port:
// device + pipeline creation, geometry submission, the camera matrices
// ported from src/ogl.c, and the per-frame scene render pass.
//
// --- Clip space -------------------------------------------------------
// SDL_GPU normalizes every backend onto the D3D12/Metal convention
// (SDL_gpu.h "Coordinate System"): NDC lower-left is (-1,-1), upper-right
// is (1,1) -- so *no Y flip* is needed relative to OpenGL -- but Z runs
// [0,1] with 0 at the near plane, where OpenGL runs [-1,1]. cglm defaults
// to the OpenGL convention, so the projection here uses the explicit
// glm_frustum_rh_zo() ("right-handed, zero-to-one") variant rather than
// glm_frustum(). That is the *only* deviation from ogl.c's matrix math.
//
// The Y-flip question was left open by Task 3.1 (see task-3.1-report.md,
// "Self-review"); resolved here against two sources: the SDL_gpu.h text
// above, and the vendored ImGui SDLGPU3 backend, whose MSL shader carries
// an explicit `out.gl_Position.y *= -1.0` that its SPIR-V (Vulkan) shader
// does not. ImGui authors its GLSL for Vulkan's Y-down NDC and un-flips
// it for SDL_GPU's Y-up convention; our shaders are authored Y-up (they
// are a port of desktop-GL shaders), so they need no flip at all.
//
// --- Recording and replay ---------------------------------------------
// geometry.c draws the way the GL frontend let it: build a little vertex
// array on the stack or the heap, hand it over, draw it, move on -- a few
// thousand times per frame, with the modelview matrix and the fill color
// changing in between. OpenGL swallows that because glBufferData and
// glDrawArrays go into one ordered stream. SDL_GPU does not: buffer
// copies are illegal inside a render pass, so the vertex data for a frame
// has to be on the GPU *before* the pass that draws it opens.
//
// Hence: gpu_draw() records rather than draws. It appends the vertices
// and (converted) indices to two CPU-side arenas and pushes a DrawCmd
// carrying the byte offsets, the pipeline and a snapshot of both uniform
// blocks. gpu_scene_end() then does the whole frame in one shot: one
// copy pass uploading both arenas, then one render pass replaying every
// DrawCmd. So a frame costs one transfer and one pass no matter how many
// nodes are on screen, and the uniform snapshots preserve the exact
// per-draw state the GL code expressed by ordering.
//
// (The alternative -- a persistent buffer per call site, uploaded on its
// own command buffer -- reorders uploads ahead of draws that were
// recorded between them, which silently paints every node with the last
// node's geometry. Cycling papers over that only by allocating a fresh
// internal buffer per draw per frame.)
//
// --- Render pass structure -------------------------------------------
// Two passes per frame, both on the swapchain texture:
//   1. scene: LOADOP_CLEAR color + depth, depth-stencil target attached.
//   2. ImGui (in main.cpp): LOADOP_LOAD, no depth target.
// Two passes rather than one because ImGui must not be depth-tested and
// must not inherit the scene pipeline's viewport/scissor state (its
// backend sets and leaks its own -- see the comment at
// imgui_impl_sdlgpu3.cpp:312). Ending the scene pass costs one extra
// encoder per frame on Metal and buys complete state isolation.

#include "gpu.h"

// cglm's <cglm/cglm.h> only pulls in the clip-space family selected by
// CGLM_CLIP_CONTROL (right-handed, negative-one-to-one by default, i.e.
// OpenGL's). SDL_GPU wants zero-to-one depth, so the *_rh_zo header has
// to be included by hand. Changing CGLM_CLIP_CONTROL project-wide would
// also silently retarget the still-OpenGL GTK frontend, which shares
// cglm — hence the explicit per-call-site variant.
#include <cglm/clipspace/ortho_rh_zo.h> /* overview mini-map, fsn-mode Task C1 */
#include <cglm/clipspace/persp_rh_zo.h>

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "gpu_internal.hpp"

extern "C" {
#include "common.h"
#include "camera.h"
#include "geometry.h" /* geometry_draw( ) -- see gpu_pick() */
#include "geometry-fsn.h" /* fsn_layout_bounds( ) -- see gpu_overview_render() */
#include "tmaptext.h" /* text_upload_mvp( ) -- see gpu_upload_matrices() */
}

#include "fsn-style.h" /* FsnLandscape, fsn_landscapes[] -- see draw_landscape() */

// Byte arrays for shaders/compiled/{scene,text}.{vert,frag}.{msl,spv},
// generated at build time by tools/embed-shaders.py.
#include "shaders_embedded.h"

// ---- Uniform blocks --------------------------------------------------
//
// Mirrors of the std140 blocks declared in shaders/src/scene.vert and
// shaders/src/scene.frag. Field order, types and padding are dictated by
// those two files -- read them before touching anything here.
//
// std140 rules that matter for these two blocks: mat4 and vec4 are
// 16-byte aligned (and mat4 is 4 columns of vec4, i.e. exactly cglm's
// mat4 memory layout, column-major); float/int are 4-byte aligned; the
// block itself is rounded up to a multiple of 16. `normal_matrix` is a
// mat4 rather than a mat3 precisely so that no per-column padding has to
// be replicated by hand (see the rationale in scene.vert's header).

namespace {

struct alignas(16) SceneVertUBO {
	float mvp[16];           // offset   0
	float modelview[16];     // offset  64
	float normal_matrix[16]; // offset 128 (upper-left 3x3 is the real one)
	float light_pos[4];      // offset 192
	std::int32_t lightning_enabled; // offset 208
	std::int32_t pad_[3];    // std140 rounds the block up to 16 bytes
};
static_assert(sizeof(SceneVertUBO) == 224,
    "SceneVertUBO must match the std140 layout of scene.vert's SceneVertUBO");
static_assert(offsetof(SceneVertUBO, modelview) == 64, "std140 offset");
static_assert(offsetof(SceneVertUBO, normal_matrix) == 128, "std140 offset");
static_assert(offsetof(SceneVertUBO, light_pos) == 192, "std140 offset");
static_assert(offsetof(SceneVertUBO, lightning_enabled) == 208, "std140 offset");

struct alignas(16) SceneFragUBO {
	float color[4];   // offset  0
	float ambient;    // offset 16
	float diffuse;    // offset 20
	float specular;   // offset 24
	std::int32_t lightning_enabled; // offset 28
};
static_assert(sizeof(SceneFragUBO) == 32,
    "SceneFragUBO must match the std140 layout of scene.frag's SceneFragUBO");
static_assert(offsetof(SceneFragUBO, ambient) == 16, "std140 offset");
static_assert(offsetof(SceneFragUBO, lightning_enabled) == 28, "std140 offset");

// FsvVertex is the pipeline's vertex input state, so its layout is a
// contract too (24-byte stride, attributes at 0 and 12).
static_assert(sizeof(FsvVertex) == 24, "scene pipeline vertex stride");
static_assert(offsetof(FsvVertex, normal) == 12, "vertex attribute 1 offset");

// Mirrors of the std140 blocks in shaders/src/text.vert / text.frag. See
// the "Text rendering" section below for the pipeline these feed.

struct alignas(16) TextVertUBO {
	float mvp[16]; // offset 0
};
static_assert(sizeof(TextVertUBO) == 64,
    "TextVertUBO must match the std140 layout of text.vert's TextVertUBO");

struct alignas(16) TextFragUBO {
	float color[3]; // offset 0
	float pad_;      // std140 rounds a lone vec3 block up to 16 bytes
};
static_assert(sizeof(TextFragUBO) == 16,
    "TextFragUBO must match the std140 layout of text.frag's TextFragUBO");

// FsvTextVertex (src/gpu.h) is the text pipeline's vertex input state.
static_assert(sizeof(FsvTextVertex) == 20, "text pipeline vertex stride");
static_assert(offsetof(FsvTextVertex, texcoord) == 12,
    "text vertex attribute 1 offset");

// ---- Module state ----------------------------------------------------

SDL_Window *g_window;
SDL_GPUDevice *g_device;

// False until gpu_init() has completed *every* step. gpu_init() returns
// void (that is the C contract geometry.c compiles against), so this is
// how main.cpp tells "renderer up" from "renderer half-built": without
// it, a failed device/shader/pipeline step would leave the app running
// and logging the same error once per frame forever.
bool g_ready;

// Kept alive for the process lifetime rather than released after
// gpu_init(): pipelines are created lazily (see pipeline_for()), so the
// shader modules are still needed the first time an unusual
// topology/depth-test combination shows up mid-frame.
SDL_GPUShader *g_vert_shader;
SDL_GPUShader *g_frag_shader;

// Lazily built pipeline cache. SDL_GPU bakes primitive type, depth
// comparison and color-target format into the pipeline object, and
// geometry.c varies all three:
//   - triangles vs lines            (every builder vs the outlines)
//   - LESS / LEQUAL / GREATER       (everything vs the node cursor)
//   - swapchain vs id-color target  (rendering vs picking, Task 4.2)
// Only combinations actually drawn get created; a normal frame builds
// two (triangles+LESS, lines+LESS) and adds the two cursor line variants
// the first time a cursor is drawn.
enum { NUM_PRIMS = 2, NUM_DEPTH_TESTS = 5, NUM_TARGETS = 2 };
SDL_GPUGraphicsPipeline *g_pipelines[NUM_PRIMS][NUM_DEPTH_TESTS][NUM_TARGETS];

SDL_GPUTexture *g_depth_texture;
SDL_GPUTextureFormat g_depth_format;
Uint32 g_depth_width, g_depth_height;

// Per-frame state, valid between gpu_frame_begin() and gpu_frame_end().
SDL_GPUCommandBuffer *g_cmd;
SDL_GPUTexture *g_swapchain;
Uint32 g_swapchain_width, g_swapchain_height;

// --screenshot/--record: when non-null, the scene renders into this
// offscreen texture instead of the swapchain (see gpu_screenshot_begin()/
// gpu_record_begin()).
SDL_GPUTexture *g_capture_texture;
Uint32 g_capture_width, g_capture_height;

// True only between gpu_record_begin() and gpu_record_end(). Distinguishes
// the two offscreen-capture callers for gpu_scene_begin()'s target_index
// pick below: --screenshot/gpu_pick's g_capture_texture is a fixed
// R8G8B8A8_UNORM format with its own pipelines (target index 1);
// --record's is deliberately created in the *swapchain's* pixel format
// (see gpu_record_begin()) so it can reuse the swapchain's own pipelines
// (target index 0) -- the same ones ImGui's backend was initialized
// against -- letting a recorded frame composite scene + ImGui exactly
// like a visible one does, with no second ImGui pipeline required.
bool g_recording_frame;

// fsn-mode Task C1: the overview mini-map's own offscreen target, plus
// the top-down orthographic frame the last render used.
//
// The fourth offscreen render path in this file, and the only *cached*
// one: gpu_pick(), --screenshot and --record each build a texture, use it
// once and release it, because each runs at most once per user action or
// per captured frame. The overview redraws whenever the camera moves and
// is sampled by ImGui every frame, so its texture (and its own
// swapchain-independent depth buffer -- ensure_depth_texture()'s single
// cached texture is sized to the *window*, and making it flip-flop
// between two sizes every frame would reallocate it twice a frame) lives
// until gpu_shutdown().
//
// It is also the only one that does NOT go through g_capture_texture:
// gpu_scene_begin()/gpu_scene_end() branch on g_overview_frame first (see
// both), so an overview render leaves every byte of the capture state
// alone. That is what lets it run inside a --record frame, which has its
// own g_capture_texture in the swapchain's format and would otherwise
// pick the wrong pipeline set for this R8G8B8A8 texture.
SDL_GPUTexture *g_overview_texture;
SDL_GPUTexture *g_overview_depth;
Uint32 g_overview_width, g_overview_height;

// True only between gpu_overview_render()'s gpu_scene_begin() and
// gpu_scene_end() -- see gpu_overview_pass(), the read side geometry-fsn-
// draw.c uses.
bool g_overview_frame;

// The world-space ground rectangle the last successful overview render
// framed, and whether there has been one. src/sdl/ui_overview.cpp maps a
// click inside the image back through this rectangle, so it has to be
// the *same* numbers the render used, not a recomputation.
double g_overview_x0, g_overview_x1, g_overview_y0, g_overview_y1;
bool g_overview_framed;

// The top-down camera's height and far plane for the render in progress,
// computed by overview_frame_scene() and consumed by
// setup_overview_matrices() one call later (from inside
// gpu_scene_begin(), which takes no arguments).
float g_overview_eye_height, g_overview_far_clip;

// Where the scene pass is currently drawing, and which of pipeline_for()'s
// two color-target formats that is.
SDL_GPUTexture *g_color_target;
int g_target_index;

// Shadow copies of the two uniform blocks: gpu_set_color() and friends
// write here, and every gpu_draw() snapshots them into its DrawCmd.
SceneVertUBO g_vert_ubo;
SceneFragUBO g_frag_ubo;

FsvDepthTest g_depth_test;
FsvRenderMode g_render_mode;

// Current landscape preset (fsn-mode Task A1): an index into
// fsn_landscapes[] (src/fsn-style.h), or FSN_LANDSCAPE_OFF. Set by
// gpu_set_landscape(), consumed once per frame by gpu_scene_begin()'s
// call to draw_landscape() below.
int g_landscape_index = FSN_LANDSCAPE_OFF;

// The base modelview matrix: right-handed, +z straight up, camera at the
// origin looking down -x. Private, unlike gl.base_modelview in ogl.h --
// nothing outside this file ever read it.
mat4 g_base_modelview;

// ---- Recorded geometry (see "Recording and replay" above) ------------

struct DrawCmd {
	SDL_GPUGraphicsPipeline *pipeline;
	Uint32 first_index;
	Uint32 num_indices;
	Sint32 vertex_offset;
	SceneVertUBO vert_ubo;
	SceneFragUBO frag_ubo;
};

std::vector<FsvVertex> g_vertices;
std::vector<Uint32> g_indices;
std::vector<DrawCmd> g_draws;

// True between gpu_scene_begin() and gpu_scene_end(), and only when the
// frame can actually be drawn. gpu_draw() is a cheap no-op otherwise.
bool g_recording;

SDL_GPUBuffer *g_vertex_buffer;
SDL_GPUBuffer *g_index_buffer;
Uint32 g_vertex_capacity; // bytes
Uint32 g_index_capacity;  // bytes
SDL_GPUTransferBuffer *g_transfer;
Uint32 g_transfer_capacity; // bytes

// ---- Text rendering (src/tmaptext.c via src/gpu.h's gpu_text_*()) ----
//
// tmaptext.c no longer touches GL directly (Task 3.4); it draws through
// the six gpu_text_*() entry points below. Text gets its own texture, its
// own pipeline (alpha-blended, no depth bias -- see text_pipeline_for())
// and its own recording arena, built and replayed the same way as the
// scene's and for the same reason: geometry.c's text_pre()/
// text_draw_*()/text_post() calls are interleaved with gpu_draw() calls
// mid-tree-walk, and SDL_GPU forbids buffer copies inside a render pass
// regardless of which pipeline the data is for.
//
// mvp and color are snapshotted per gpu_text_draw() call, not once per
// frame: TreeV's label pass re-walks the tree with a fresh
// gpu_upload_matrices() (hence a fresh text_upload_mvp()) and a fresh
// text_set_color() at every node -- see mapv_draw_recursive()/
// treev_draw_recursive() in geometry.c. A single frame-wide uniform push
// would paint every label with the last node's color and transform.

SDL_GPUShader *g_text_vert_shader;
SDL_GPUShader *g_text_frag_shader;

// One pipeline per color-target format (swapchain vs the offscreen
// R8G8B8A8 capture/id target), same NUM_TARGETS dimension as the scene's
// g_pipelines. No primitive-type or depth-test dimension: text is always
// triangles, always FSV_DEPTH_LESS (geometry.c never changes the depth
// function around a label).
SDL_GPUGraphicsPipeline *g_text_pipelines[NUM_TARGETS];

// The glyph atlas (one texture, uploaded once by gpu_text_init()) and its
// sampler.
SDL_GPUTexture *g_text_texture;
SDL_GPUSampler *g_text_sampler;

// Shadow copies of the text uniform blocks, written by gpu_text_set_color()
// / gpu_text_upload_mvp() and snapshotted into a TextDrawCmd by every
// gpu_text_draw().
TextVertUBO g_text_vert_ubo;
TextFragUBO g_text_frag_ubo;

struct TextDrawCmd {
	Uint32 first_index;
	Uint32 num_indices;
	Sint32 vertex_offset;
	TextVertUBO vert_ubo;
	TextFragUBO frag_ubo;
};

std::vector<FsvTextVertex> g_text_vertices;
std::vector<Uint32> g_text_indices;
std::vector<TextDrawCmd> g_text_draws;

SDL_GPUBuffer *g_text_vertex_buffer;
SDL_GPUBuffer *g_text_index_buffer;
Uint32 g_text_vertex_capacity; // bytes
Uint32 g_text_index_capacity;  // bytes
SDL_GPUTransferBuffer *g_text_transfer;
Uint32 g_text_transfer_capacity; // bytes

// ---- Shaders and pipelines -------------------------------------------

// Returns the viewport's current aspect ratio (width / height). Port of
// ogl_aspect_ratio(), which read it back out of glGetIntegerv(GL_VIEWPORT).
double
gpu_aspect_ratio(void)
{
	int w = 0, h = 0;
	SDL_GetWindowSizeInPixels(g_window, &w, &h);
	if (h <= 0)
		return 1.0;
	return (double)w / (double)h;
}

struct ShaderBlob {
	SDL_GPUShaderFormat format;
	const char *entrypoint;
	const unsigned char *code;
	size_t size;
	const char *name;
};

// Creates one shader stage, preferring MSL (native on Metal) and falling
// back to whatever else both the device and Task 3.1 produced. The MSL
// artifacts have never been through a real Metal compiler -- this call is
// their first (see task-3.1-report.md) -- so a failure is logged in full
// rather than swallowed, and the next candidate format is tried.
SDL_GPUShader *
create_shader(SDL_GPUShaderStage stage, const ShaderBlob *candidates,
    int num_candidates, Uint32 num_uniform_buffers, Uint32 num_samplers = 0)
{
	const SDL_GPUShaderFormat supported = SDL_GetGPUShaderFormats(g_device);

	for (int i = 0; i < num_candidates; i++) {
		const ShaderBlob &blob = candidates[i];
		if (!(supported & blob.format))
			continue;

		SDL_GPUShaderCreateInfo info = {};
		info.code = blob.code;
		info.code_size = blob.size;
		info.entrypoint = blob.entrypoint;
		info.format = blob.format;
		info.stage = stage;
		info.num_samplers = num_samplers;
		info.num_storage_textures = 0;
		info.num_storage_buffers = 0;
		info.num_uniform_buffers = num_uniform_buffers;

		SDL_GPUShader *shader = SDL_CreateGPUShader(g_device, &info);
		if (shader != nullptr) {
			SDL_Log("gpu: loaded %s (%zu bytes, entry \"%s\")",
			    blob.name, blob.size, blob.entrypoint);
			return shader;
		}
		SDL_Log("gpu: SDL_CreateGPUShader failed for %s: %s",
		    blob.name, SDL_GetError());
	}
	return nullptr;
}

// Returns the pipeline for one (primitive type, depth test, color target)
// combination, building it on first use.
SDL_GPUGraphicsPipeline *
pipeline_for(SDL_GPUPrimitiveType prim, FsvDepthTest depth_test, int target)
{
	const int prim_index = prim == SDL_GPU_PRIMITIVETYPE_LINELIST ? 1 : 0;
	SDL_GPUGraphicsPipeline *&slot =
	    g_pipelines[prim_index][depth_test][target];
	if (slot != nullptr)
		return slot;

	SDL_GPUVertexBufferDescription vertex_buffer_desc = {};
	vertex_buffer_desc.slot = 0;
	vertex_buffer_desc.pitch = sizeof(FsvVertex);
	vertex_buffer_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
	vertex_buffer_desc.instance_step_rate = 0;

	SDL_GPUVertexAttribute vertex_attributes[2] = {};
	vertex_attributes[0].location = 0; // scene.vert: in vec3 position
	vertex_attributes[0].buffer_slot = 0;
	vertex_attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
	vertex_attributes[0].offset = offsetof(FsvVertex, pos);
	vertex_attributes[1].location = 1; // scene.vert: in vec3 normal
	vertex_attributes[1].buffer_slot = 0;
	vertex_attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
	vertex_attributes[1].offset = offsetof(FsvVertex, normal);

	SDL_GPUVertexInputState vertex_input_state = {};
	vertex_input_state.vertex_buffer_descriptions = &vertex_buffer_desc;
	vertex_input_state.num_vertex_buffers = 1;
	vertex_input_state.vertex_attributes = vertex_attributes;
	vertex_input_state.num_vertex_attributes = 2;

	// ogl_init(): glEnable(GL_CULL_FACE) with GL's defaults, i.e. cull
	// back faces, front faces wound counter-clockwise.
	SDL_GPURasterizerState rasterizer_state = {};
	rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
	rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
	rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

	// GL always clips primitives against the near/far planes; SDL_GPU's
	// zero-initialized default here is the OPPOSITE (false == depth
	// *clamp*, Metal's MTLDepthClipModeClamp / Vulkan's depthClampEnable).
	// Leaving it off kept geometry between the eye and the near plane --
	// which GL discards -- fully rasterized, so on any close approach the
	// foreground boxes GL would clip away instead walled off the whole
	// frame and occluded everything behind them. That was the entire
	// "spotlight cone stops rendering below ratio ~2.0-2.3" mystery
	// (TODO.md): the threshold was exactly where the near plane
	// (NEAR_TO_DISTANCE_RATIO: half the camera-to-target distance) starts
	// overlapping the selected pedestal's file boxes -- and it also
	// explains why manipulating the clip planes during that investigation
	// changed nothing: in clamp mode they clip nothing. The ground quad
	// used to lean on clamp mode to survive the far plane; it is sized to
	// the frustum in draw_landscape() now, so nothing here needs clamping.
	rasterizer_state.enable_depth_clip = true;

	// ogl_init(): glEnable(GL_POLYGON_OFFSET_FILL) + glPolygonOffset(1,1),
	// which in GL applies to filled polygons only -- so the line
	// pipelines deliberately do not carry it. Same factor/units, same
	// sign convention (positive pushes fills away from the viewer), so
	// the black folder outlines and cursor bars that sit exactly on a
	// face's plane win the depth test instead of z-fighting with it.
	if (prim == SDL_GPU_PRIMITIVETYPE_TRIANGLELIST) {
		rasterizer_state.enable_depth_bias = true;
		rasterizer_state.depth_bias_constant_factor = 1.0f;
		rasterizer_state.depth_bias_slope_factor = 1.0f;
		rasterizer_state.depth_bias_clamp = 0.0f;
	}

	// ogl_init(): glEnable(GL_DEPTH_TEST) with GL's default GL_LESS;
	// geometry.c's cursor switches to GL_GREATER/GL_LEQUAL around its
	// two halves (cursor_hidden_part()/cursor_visible_part()).
	SDL_GPUDepthStencilState depth_stencil_state = {};
	depth_stencil_state.enable_stencil_test = false;
	if (depth_test == FSV_DEPTH_ALWAYS_NOWRITE) {
		// fsn-mode landscape sky (Task A1): must always draw (never
		// itself discarded) and must never leave a depth value behind
		// for a later draw to lose to -- see gpu.h's FsvDepthTest
		// comment for why a fixed near-1.0 NDC depth was rejected in
		// favor of this. Disabling the test outright, rather than
		// leaving it enabled with SDL_GPU_COMPAREOP_ALWAYS, is what
		// actually guarantees the "never writes" half on every backend:
		// SDL_GPU mirrors Vulkan/Metal's rule that a depth write only
		// takes effect while the depth test itself is enabled, whatever
		// enable_depth_write says -- so this states that rule instead
		// of leaning on it implicitly.
		depth_stencil_state.enable_depth_test = false;
		depth_stencil_state.enable_depth_write = false;
	} else if (depth_test == FSV_DEPTH_LESS_NOWRITE) {
		// fsn-mode selection spotlight (Task B3): test against the real
		// depth buffer (LESS, same comparison as ordinary opaque scene
		// geometry) but never write to it -- see gpu.h's FsvDepthTest
		// comment for why this is the opposite trade-off from
		// FSV_DEPTH_ALWAYS_NOWRITE above, and why it is also the variant
		// that enables blending below.
		depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
		depth_stencil_state.enable_depth_test = true;
		depth_stencil_state.enable_depth_write = false;
	} else {
		switch (depth_test) {
			case FSV_DEPTH_LEQUAL:
			depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
			break;
			case FSV_DEPTH_GREATER:
			depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER;
			break;
			default:
			depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
			break;
		}
		depth_stencil_state.enable_depth_test = true;
		depth_stencil_state.enable_depth_write = true;
	}

	// No blending for ordinary scene geometry: the GL frontend sets a
	// blend func but never enables GL_BLEND for it (only the text overlay
	// used to, Task 3.4's pipeline). FSV_DEPTH_LESS_NOWRITE is the one
	// exception -- gpu.h's FsvDepthTest comment explains why blending
	// rides on this particular depth-test value rather than getting its
	// own gpu_draw() parameter -- and uses the same SRC_ALPHA /
	// ONE_MINUS_SRC_ALPHA factors as text_pipeline_for()'s blend_state
	// below, for the same reason: the GL original's one ogl_init()
	// glBlendFunc() call is shared by everything translucent.
	SDL_GPUColorTargetBlendState blend_state = {};
	blend_state.enable_blend = false;
	if (depth_test == FSV_DEPTH_LESS_NOWRITE) {
		blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		blend_state.dst_color_blendfactor =
		    SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
		blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		blend_state.dst_alpha_blendfactor =
		    SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
		blend_state.enable_blend = true;
	}

	SDL_GPUColorTargetDescription color_target_desc = {};
	color_target_desc.format = target == 0
	    ? SDL_GetGPUSwapchainTextureFormat(g_device, g_window)
	    : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	color_target_desc.blend_state = blend_state;

	SDL_GPUGraphicsPipelineTargetInfo target_info = {};
	target_info.color_target_descriptions = &color_target_desc;
	target_info.num_color_targets = 1;
	target_info.depth_stencil_format = g_depth_format;
	target_info.has_depth_stencil_target = true;

	SDL_GPUGraphicsPipelineCreateInfo info = {};
	info.vertex_shader = g_vert_shader;
	info.fragment_shader = g_frag_shader;
	info.vertex_input_state = vertex_input_state;
	info.primitive_type = prim;
	info.rasterizer_state = rasterizer_state;
	info.depth_stencil_state = depth_stencil_state;
	info.target_info = target_info;

	slot = SDL_CreateGPUGraphicsPipeline(g_device, &info);
	if (slot == nullptr)
		SDL_Log("gpu: pipeline creation failed (prim %d, depth %d, "
		    "target %d): %s", (int)prim, (int)depth_test, target,
		    SDL_GetError());
	return slot;
}

// Returns the text pipeline for one color-target format, building it on
// first use. See the "Text rendering" module-state comment above for why
// this is a much smaller cache than pipeline_for()'s.
SDL_GPUGraphicsPipeline *
text_pipeline_for(int target)
{
	SDL_GPUGraphicsPipeline *&slot = g_text_pipelines[target];
	if (slot != nullptr)
		return slot;

	SDL_GPUVertexBufferDescription vertex_buffer_desc = {};
	vertex_buffer_desc.slot = 0;
	vertex_buffer_desc.pitch = sizeof(FsvTextVertex);
	vertex_buffer_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

	SDL_GPUVertexAttribute vertex_attributes[2] = {};
	vertex_attributes[0].location = 0; // text.vert: in vec3 position
	vertex_attributes[0].buffer_slot = 0;
	vertex_attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
	vertex_attributes[0].offset = offsetof(FsvTextVertex, pos);
	vertex_attributes[1].location = 1; // text.vert: in vec2 texcoord
	vertex_attributes[1].buffer_slot = 0;
	vertex_attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
	vertex_attributes[1].offset = offsetof(FsvTextVertex, texcoord);

	SDL_GPUVertexInputState vertex_input_state = {};
	vertex_input_state.vertex_buffer_descriptions = &vertex_buffer_desc;
	vertex_input_state.num_vertex_buffers = 1;
	vertex_input_state.vertex_attributes = vertex_attributes;
	vertex_input_state.num_vertex_attributes = 2;

	// Same cull/winding as the scene pipeline: ogl_init()'s
	// glEnable(GL_CULL_FACE) was never disabled around text --
	// text_pre()/text_post() only ever touched GL_POLYGON_OFFSET_FILL
	// and GL_BLEND. No depth bias here: GL_POLYGON_OFFSET_FILL applies
	// to filled *scene* polygons and text_pre() explicitly disabled it
	// for the duration of every text draw, so the text pipeline simply
	// never carries the depth-bias fields pipeline_for() sets for
	// triangles.
	SDL_GPURasterizerState rasterizer_state = {};
	rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
	rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
	rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
	// GL-style near/far clipping, not SDL_GPU's zero-init depth clamp --
	// see pipeline_for()'s comment on the same line for the full story.
	rasterizer_state.enable_depth_clip = true;

	// Depth test ON, depth WRITE ON. This matches the GL original
	// exactly rather than the "depth write off" pattern that is typical
	// for text-over-geometry: grep -n "glDepthMask" src/*.c is empty, so
	// the old code never touched the depth mask anywhere, meaning text
	// drew with GL's default (write enabled) the whole time, same as
	// scene geometry. See docs/PORTING.md Task 3.4 for what the
	// "typical" alternative would have changed.
	SDL_GPUDepthStencilState depth_stencil_state = {};
	depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
	depth_stencil_state.enable_depth_test = true;
	depth_stencil_state.enable_depth_write = true;
	depth_stencil_state.enable_stencil_test = false;

	// Alpha-blended: ogl_init()'s single glBlendFunc(GL_SRC_ALPHA,
	// GL_ONE_MINUS_SRC_ALPHA) call (the GL original never calls
	// glBlendFuncSeparate, so color and alpha share one factor pair)
	// plus text_pre()'s glEnable(GL_BLEND).
	SDL_GPUColorTargetBlendState blend_state = {};
	blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
	blend_state.dst_color_blendfactor =
	    SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
	blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
	blend_state.dst_alpha_blendfactor =
	    SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
	blend_state.enable_blend = true;

	SDL_GPUColorTargetDescription color_target_desc = {};
	color_target_desc.format = target == 0
	    ? SDL_GetGPUSwapchainTextureFormat(g_device, g_window)
	    : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	color_target_desc.blend_state = blend_state;

	SDL_GPUGraphicsPipelineTargetInfo target_info = {};
	target_info.color_target_descriptions = &color_target_desc;
	target_info.num_color_targets = 1;
	target_info.depth_stencil_format = g_depth_format;
	target_info.has_depth_stencil_target = true;

	SDL_GPUGraphicsPipelineCreateInfo info = {};
	info.vertex_shader = g_text_vert_shader;
	info.fragment_shader = g_text_frag_shader;
	info.vertex_input_state = vertex_input_state;
	info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	info.rasterizer_state = rasterizer_state;
	info.depth_stencil_state = depth_stencil_state;
	info.target_info = target_info;

	slot = SDL_CreateGPUGraphicsPipeline(g_device, &info);
	if (slot == nullptr)
		SDL_Log("gpu: text pipeline creation failed (target %d): %s",
		    target, SDL_GetError());
	return slot;
}

// Picks the first depth format the device supports as a depth target.
bool
choose_depth_format(void)
{
	const SDL_GPUTextureFormat candidates[] = {
		SDL_GPU_TEXTUREFORMAT_D24_UNORM,
		SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
	};
	const char *names[] = { "D24_UNORM", "D32_FLOAT" };

	for (size_t i = 0; i < SDL_arraysize(candidates); i++) {
		if (SDL_GPUTextureSupportsFormat(g_device, candidates[i],
		        SDL_GPU_TEXTURETYPE_2D,
		        SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
			g_depth_format = candidates[i];
			SDL_Log("gpu: depth format %s", names[i]);
			return true;
		}
	}
	SDL_Log("gpu: no supported depth texture format");
	return false;
}

// (Re)creates the depth buffer to match the swapchain size. SDL defers
// the actual release until the GPU is done with the old texture, so this
// is safe to call mid-frame after acquiring the swapchain image.
bool
ensure_depth_texture(Uint32 width, Uint32 height)
{
	if (g_depth_texture != nullptr && g_depth_width == width &&
	    g_depth_height == height)
		return true;

	if (g_depth_texture != nullptr)
		SDL_ReleaseGPUTexture(g_device, g_depth_texture);

	SDL_GPUTextureCreateInfo info = {};
	info.type = SDL_GPU_TEXTURETYPE_2D;
	info.format = g_depth_format;
	info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
	info.width = width;
	info.height = height;
	info.layer_count_or_depth = 1;
	info.num_levels = 1;
	info.sample_count = SDL_GPU_SAMPLECOUNT_1;

	g_depth_texture = SDL_CreateGPUTexture(g_device, &info);
	if (g_depth_texture == nullptr) {
		SDL_Log("gpu: SDL_CreateGPUTexture (depth) failed: %s",
		    SDL_GetError());
		g_depth_width = g_depth_height = 0;
		return false;
	}
	g_depth_width = width;
	g_depth_height = height;
	return true;
}

// Grows `*buffer` to at least `size` bytes, reallocating only when it has
// to. Returns false if the (re)allocation failed.
bool
ensure_buffer(SDL_GPUBuffer **buffer, Uint32 *capacity, Uint32 size,
    SDL_GPUBufferUsageFlags usage)
{
	if (*buffer != nullptr && *capacity >= size)
		return true;

	if (*buffer != nullptr)
		SDL_ReleaseGPUBuffer(g_device, *buffer);

	SDL_GPUBufferCreateInfo info = {};
	info.usage = usage;
	info.size = size;

	*buffer = SDL_CreateGPUBuffer(g_device, &info);
	if (*buffer == nullptr) {
		SDL_Log("gpu: SDL_CreateGPUBuffer failed: %s", SDL_GetError());
		*capacity = 0;
		return false;
	}
	*capacity = size;
	return true;
}

// ---- Camera matrices (ported from src/ogl.c) -------------------------

// Port of setup_projection_matrix() (src/ogl.c:255). The original took a
// `full_reset` flag so a pick matrix could be multiplied in; every call
// site passed TRUE once gluPickMatrix() was dropped, so the parameter is
// gone. The math is otherwise unchanged apart from the zero-to-one depth
// range (see the clip-space note at the top of this file).
void
setup_projection_matrix(void)
{
	// Before camera_init() runs (nothing has been scanned yet) the
	// camera is all zeroes, and a frustum with near == far == 0 is a
	// division by zero. Leave the projection at identity until the
	// camera is real; there is nothing to draw yet either way.
	if (!(camera->near_clip > 0.0 && camera->far_clip > camera->near_clip)) {
		glm_mat4_identity(gpu_mat.projection);
		return;
	}

	double dx, dy;

	dx = camera->near_clip * tan(0.5 * RAD(camera->fov));
	dy = dx / gpu_aspect_ratio();

	// ogl.c built the frustum into a temporary, reset gl.projection to
	// identity and multiplied the two; with `full_reset` gone that is
	// identity * frustum, so the frustum is written straight out.
	glm_frustum_rh_zo(-dx, dx, -dy, dy, camera->near_clip,
	    camera->far_clip, gpu_mat.projection);
}

// Port of setup_modelview_matrix() (src/ogl.c:273). Same operations in
// the same order with the same signs; the only edit is mechanical, since
// C compound literals ((vec3){...}) are not C++: each one becomes a named
// local, which is why the cases carry braces.
void
setup_modelview_matrix(void)
{
	glm_mat4_copy(g_base_modelview, gpu_mat.modelview);

	const float dolly = -(float)camera->distance;

	switch (globals.fsv_mode) {
		case FSV_SPLASH:
		break;

		case FSV_DISCV: {
		vec3 back = { dolly, 0.f, 0.f };
		vec3 target = { -(float)DISCV_CAMERA(camera)->target.x,
				-(float)DISCV_CAMERA(camera)->target.y,
				0.f };
		glm_translate(gpu_mat.modelview, back);
		glm_rotate_y(gpu_mat.modelview, M_PI_2, gpu_mat.modelview);
		glm_rotate_z(gpu_mat.modelview, M_PI_2, gpu_mat.modelview);
		glm_translate(gpu_mat.modelview, target);
		break;
		}

		// fsn-mode Task B1: FSN's camera target is Cartesian and lives
		// in the same MapVCamera storage (see the FSN_CAMERA_* note in
		// camera.c), so the transform is identical.
		case FSV_FSN:
		case FSV_MAPV: {
		vec3 back = { dolly, 0.f, 0.f };
		vec3 target = { -(float)MAPV_CAMERA(camera)->target.x,
				-(float)MAPV_CAMERA(camera)->target.y,
				-(float)MAPV_CAMERA(camera)->target.z };
		glm_translate(gpu_mat.modelview, back);
		glm_rotate_y(gpu_mat.modelview, camera->phi * M_PI / 180, gpu_mat.modelview);
		glm_rotate_z(gpu_mat.modelview, -camera->theta * M_PI / 180, gpu_mat.modelview);
		glm_translate(gpu_mat.modelview, target);
		break;
		}

		case FSV_TREEV: {
		vec3 back = { dolly, 0.f, 0.f };
		vec3 target = { (float)TREEV_CAMERA(camera)->target.r,
				0.0f,
				-(float)TREEV_CAMERA(camera)->target.z };
		glm_translate(gpu_mat.modelview, back);
		glm_rotate_y(gpu_mat.modelview, camera->phi * M_PI / 180, gpu_mat.modelview);
		glm_rotate_z(gpu_mat.modelview, -camera->theta * M_PI / 180, gpu_mat.modelview);
		glm_translate(gpu_mat.modelview, target);
		glm_rotate_z(gpu_mat.modelview,
			     (180.0 - TREEV_CAMERA(camera)->target.theta) * M_PI / 180,
			     gpu_mat.modelview);
		break;
		}

		// Unlike ogl.c's SWITCH_FAIL, FSV_NONE is reachable here:
		// the SDL frontend renders frames before anything has been
		// scanned, where the GTK one only ever drew from inside a
		// GtkGLArea that did not exist until a tree was loaded.
		default:
		break;
	}
}

// ---- Topology conversion --------------------------------------------
//
// SDL_GPU offers TRIANGLELIST/TRIANGLESTRIP/LINELIST/LINESTRIP/POINTLIST
// and no fans or loops, and bakes the choice into the pipeline. Rather
// than carry four pipelines per depth-test just to spell the same
// geometry four ways, everything is expanded into indices against two:
// TRIANGLELIST and LINELIST. Index expansion is a handful of integer
// appends over data that is being copied anyway, and it halves the
// pipeline count (a strip pipeline could not be shared by the fan or the
// loop in any case).

void
append_indices(FsvTopology topology, int nverts, const unsigned int *indices,
    int nindices)
{
	const Uint32 n = (Uint32)nverts;

	if (indices != nullptr) {
		// Only geometry.c's already-indexed GL_TRIANGLES draws pass
		// explicit indices; the strip/fan/loop expansions below own
		// the index order for everything else.
		SDL_assert(topology == FSV_TRIANGLES);
		g_indices.insert(g_indices.end(), indices, indices + nindices);
		return;
	}

	switch (topology) {
		case FSV_TRIANGLES:
		// No caller today: geometry.c's GL_TRIANGLES draws are all
		// indexed and took the branch above. Truncate rather than
		// emit a partial triangle if one ever arrives non-indexed.
		SDL_assert(n % 3 == 0);
		for (Uint32 i = 0; i + 2 < n; i += 3) {
			g_indices.push_back(i);
			g_indices.push_back(i + 1);
			g_indices.push_back(i + 2);
		}
		break;

		case FSV_TRIANGLE_FAN:
		// GL fan: (v0, vi, vi+1). Winding matches GL's exactly.
		for (Uint32 i = 1; i + 1 < n; i++) {
			g_indices.push_back(0);
			g_indices.push_back(i);
			g_indices.push_back(i + 1);
		}
		break;

		case FSV_TRIANGLE_STRIP:
		// GL strip: even triangles are (i, i+1, i+2), odd ones swap
		// the first two so that every triangle keeps the same
		// winding. Getting this backwards would cull exactly half of
		// each strip.
		for (Uint32 i = 0; i + 2 < n; i++) {
			if ((i & 1) == 0) {
				g_indices.push_back(i);
				g_indices.push_back(i + 1);
			} else {
				g_indices.push_back(i + 1);
				g_indices.push_back(i);
			}
			g_indices.push_back(i + 2);
		}
		break;

		case FSV_LINES:
		for (Uint32 i = 0; i + 1 < n; i += 2) {
			g_indices.push_back(i);
			g_indices.push_back(i + 1);
		}
		break;

		case FSV_LINE_STRIP:
		for (Uint32 i = 0; i + 1 < n; i++) {
			g_indices.push_back(i);
			g_indices.push_back(i + 1);
		}
		break;

		case FSV_LINE_LOOP:
		for (Uint32 i = 0; i + 1 < n; i++) {
			g_indices.push_back(i);
			g_indices.push_back(i + 1);
		}
		if (n > 2) {
			g_indices.push_back(n - 1);
			g_indices.push_back(0);
		}
		break;
	}
}

// Uploads the frame's two arenas in a single copy pass on the frame's
// command buffer. Returns false if nothing can be drawn.
bool
upload_frame_geometry(void)
{
	const Uint32 vertex_bytes =
	    (Uint32)(g_vertices.size() * sizeof(FsvVertex));
	const Uint32 index_bytes = (Uint32)(g_indices.size() * sizeof(Uint32));

	if (!ensure_buffer(&g_vertex_buffer, &g_vertex_capacity, vertex_bytes,
	        SDL_GPU_BUFFERUSAGE_VERTEX) ||
	    !ensure_buffer(&g_index_buffer, &g_index_capacity, index_bytes,
	        SDL_GPU_BUFFERUSAGE_INDEX))
		return false;

	const Uint32 total = vertex_bytes + index_bytes;
	if (g_transfer == nullptr || g_transfer_capacity < total) {
		if (g_transfer != nullptr)
			SDL_ReleaseGPUTransferBuffer(g_device, g_transfer);
		SDL_GPUTransferBufferCreateInfo transfer_info = {};
		transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		transfer_info.size = total;
		g_transfer = SDL_CreateGPUTransferBuffer(g_device, &transfer_info);
		if (g_transfer == nullptr) {
			SDL_Log("gpu: SDL_CreateGPUTransferBuffer failed: %s",
			    SDL_GetError());
			g_transfer_capacity = 0;
			return false;
		}
		g_transfer_capacity = total;
	}

	// cycle = true: the previous frame may still be reading this
	// transfer buffer when the next one starts filling it.
	void *mapped = SDL_MapGPUTransferBuffer(g_device, g_transfer, true);
	if (mapped == nullptr) {
		SDL_Log("gpu: SDL_MapGPUTransferBuffer failed: %s",
		    SDL_GetError());
		return false;
	}
	memcpy(mapped, g_vertices.data(), vertex_bytes);
	memcpy((char *)mapped + vertex_bytes, g_indices.data(), index_bytes);
	SDL_UnmapGPUTransferBuffer(g_device, g_transfer);

	SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(g_cmd);

	SDL_GPUTransferBufferLocation source = {};
	SDL_GPUBufferRegion destination = {};

	// cycle = true on both, for the same reason: an in-flight frame may
	// still be reading last frame's contents out of these buffers. The
	// render pass below is recorded *after* this copy pass, so its
	// vertex/index bindings resolve to the freshly cycled allocation.
	source.transfer_buffer = g_transfer;
	source.offset = 0;
	destination.buffer = g_vertex_buffer;
	destination.offset = 0;
	destination.size = vertex_bytes;
	SDL_UploadToGPUBuffer(copy_pass, &source, &destination, true);

	source.offset = vertex_bytes;
	destination.buffer = g_index_buffer;
	destination.offset = 0;
	destination.size = index_bytes;
	SDL_UploadToGPUBuffer(copy_pass, &source, &destination, true);

	SDL_EndGPUCopyPass(copy_pass);
	return true;
}

// Replays the frame's DrawCmd list into `pass`.
void
replay_draws(SDL_GPURenderPass *pass)
{
	SDL_GPUBufferBinding vertex_binding = {};
	vertex_binding.buffer = g_vertex_buffer;
	SDL_BindGPUVertexBuffers(pass, 0, &vertex_binding, 1);

	SDL_GPUBufferBinding index_binding = {};
	index_binding.buffer = g_index_buffer;
	SDL_BindGPUIndexBuffer(pass, &index_binding,
	    SDL_GPU_INDEXELEMENTSIZE_32BIT);

	SDL_GPUGraphicsPipeline *bound = nullptr;
	for (const DrawCmd &cmd : g_draws) {
		if (cmd.pipeline != bound) {
			SDL_BindGPUGraphicsPipeline(pass, cmd.pipeline);
			bound = cmd.pipeline;
		}
		SDL_PushGPUVertexUniformData(g_cmd, 0, &cmd.vert_ubo,
		    sizeof cmd.vert_ubo);
		SDL_PushGPUFragmentUniformData(g_cmd, 0, &cmd.frag_ubo,
		    sizeof cmd.frag_ubo);
		SDL_DrawGPUIndexedPrimitives(pass, cmd.num_indices, 1,
		    cmd.first_index, cmd.vertex_offset, 0);
	}
}

// Uploads the frame's text arenas in their own copy pass, the same shape
// as upload_frame_geometry() but into the text buffers. A second, separate
// transfer buffer rather than sharing the geometry one: text data is tiny
// (a few hundred labels at most) and keeping the two independent avoids
// coupling their capacity growth for no benefit.
bool
upload_frame_text_geometry(void)
{
	const Uint32 vertex_bytes =
	    (Uint32)(g_text_vertices.size() * sizeof(FsvTextVertex));
	const Uint32 index_bytes =
	    (Uint32)(g_text_indices.size() * sizeof(Uint32));

	if (!ensure_buffer(&g_text_vertex_buffer, &g_text_vertex_capacity,
	        vertex_bytes, SDL_GPU_BUFFERUSAGE_VERTEX) ||
	    !ensure_buffer(&g_text_index_buffer, &g_text_index_capacity,
	        index_bytes, SDL_GPU_BUFFERUSAGE_INDEX))
		return false;

	const Uint32 total = vertex_bytes + index_bytes;
	if (g_text_transfer == nullptr || g_text_transfer_capacity < total) {
		if (g_text_transfer != nullptr)
			SDL_ReleaseGPUTransferBuffer(g_device, g_text_transfer);
		SDL_GPUTransferBufferCreateInfo transfer_info = {};
		transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		transfer_info.size = total;
		g_text_transfer =
		    SDL_CreateGPUTransferBuffer(g_device, &transfer_info);
		if (g_text_transfer == nullptr) {
			SDL_Log("gpu: text SDL_CreateGPUTransferBuffer failed: %s",
			    SDL_GetError());
			g_text_transfer_capacity = 0;
			return false;
		}
		g_text_transfer_capacity = total;
	}

	void *mapped = SDL_MapGPUTransferBuffer(g_device, g_text_transfer, true);
	if (mapped == nullptr) {
		SDL_Log("gpu: text SDL_MapGPUTransferBuffer failed: %s",
		    SDL_GetError());
		return false;
	}
	memcpy(mapped, g_text_vertices.data(), vertex_bytes);
	memcpy((char *)mapped + vertex_bytes, g_text_indices.data(), index_bytes);
	SDL_UnmapGPUTransferBuffer(g_device, g_text_transfer);

	SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(g_cmd);

	SDL_GPUTransferBufferLocation source = {};
	SDL_GPUBufferRegion destination = {};

	source.transfer_buffer = g_text_transfer;
	source.offset = 0;
	destination.buffer = g_text_vertex_buffer;
	destination.offset = 0;
	destination.size = vertex_bytes;
	SDL_UploadToGPUBuffer(copy_pass, &source, &destination, true);

	source.offset = vertex_bytes;
	destination.buffer = g_text_index_buffer;
	destination.offset = 0;
	destination.size = index_bytes;
	SDL_UploadToGPUBuffer(copy_pass, &source, &destination, true);

	SDL_EndGPUCopyPass(copy_pass);
	return true;
}

// Replays the frame's text draws into `pass`, after replay_draws() and
// before the pass ends -- same render pass, different pipeline and vertex/
// index buffers, which SDL_GPU allows rebinding mid-pass. Depth-testing
// against the just-replayed scene geometry's depth buffer is exactly the
// point: it is what lets a label be correctly hidden behind whatever
// geometry is actually nearest, regardless of which was recorded first.
void
replay_text_draws(SDL_GPURenderPass *pass)
{
	SDL_GPUGraphicsPipeline *pipeline = text_pipeline_for(g_target_index);
	if (pipeline == nullptr)
		return;

	SDL_GPUBufferBinding vertex_binding = {};
	vertex_binding.buffer = g_text_vertex_buffer;
	SDL_BindGPUVertexBuffers(pass, 0, &vertex_binding, 1);

	SDL_GPUBufferBinding index_binding = {};
	index_binding.buffer = g_text_index_buffer;
	SDL_BindGPUIndexBuffer(pass, &index_binding,
	    SDL_GPU_INDEXELEMENTSIZE_32BIT);

	SDL_BindGPUGraphicsPipeline(pass, pipeline);

	SDL_GPUTextureSamplerBinding sampler_binding = {};
	sampler_binding.texture = g_text_texture;
	sampler_binding.sampler = g_text_sampler;
	SDL_BindGPUFragmentSamplers(pass, 0, &sampler_binding, 1);

	for (const TextDrawCmd &cmd : g_text_draws) {
		SDL_PushGPUVertexUniformData(g_cmd, 0, &cmd.vert_ubo,
		    sizeof cmd.vert_ubo);
		SDL_PushGPUFragmentUniformData(g_cmd, 0, &cmd.frag_ubo,
		    sizeof cmd.frag_ubo);
		SDL_DrawGPUIndexedPrimitives(pass, cmd.num_indices, 1,
		    cmd.first_index, cmd.vertex_offset, 0);
	}
}

} // namespace

// ---- Public API ------------------------------------------------------

FsvGpuMatrices gpu_mat;

// Port of ogl_upload_matrices() (src/ogl.c:318). The GL original ended in
// glUniformMatrix4fv(); here the values land in the shadow UBO and are
// snapshotted by the next gpu_draw(), which means this stays callable
// outside a render pass (geometry.c calls it while walking the tree).
void
gpu_upload_matrices(void)
{
	mat4 mvp;
	glm_mat4_mul(gpu_mat.projection, gpu_mat.modelview, mvp);

	mat3 normmat;
	glm_mat4_pick3(gpu_mat.modelview, normmat);
	glm_mat3_inv(normmat, normmat);
	glm_mat3_transpose(normmat);

	// std140 wants the normal matrix as a mat4 (see scene.vert): copy
	// the 3x3 into the upper-left corner of an identity matrix.
	mat4 normmat4;
	glm_mat4_identity(normmat4);
	glm_mat4_ins3(normmat, normmat4);

	memcpy(g_vert_ubo.mvp, mvp, sizeof mvp);
	memcpy(g_vert_ubo.modelview, gpu_mat.modelview, sizeof(mat4));
	memcpy(g_vert_ubo.normal_matrix, normmat4, sizeof normmat4);

	// Port of ogl-gpu-compat.c's identical call: the text engine's mvp is
	// always refreshed alongside the scene's (see gpu.h's note on
	// gpu_text_upload_mvp()). about_splash_draw() -- GTK-only, no SDL_GPU
	// equivalent -- is the one caller that wants something different and
	// calls text_upload_mvp() directly afterwards.
	text_upload_mvp((float *)mvp);
}

void
gpu_set_color(float r, float g, float b, float a)
{
	g_frag_ubo.color[0] = r;
	g_frag_ubo.color[1] = g;
	g_frag_ubo.color[2] = b;
	g_frag_ubo.color[3] = a;
}

void
gpu_set_lighting(int enabled)
{
	g_vert_ubo.lightning_enabled = enabled ? 1 : 0;
	g_frag_ubo.lightning_enabled = enabled ? 1 : 0;
}

void
gpu_set_depth_test(FsvDepthTest test)
{
	g_depth_test = test;
}

// No-op by design. SDL_GPU has no line-width control: every backend
// rasterizes lines exactly one pixel wide (there is no field for it in
// SDL_GPURasterizerState, and Metal has no equivalent of glLineWidth at
// all). geometry.c's 2/3/5-pixel requests are therefore drawn 1 pixel
// wide here; the GL shim still honors them, so the GTK frontend is
// unchanged. See docs/PORTING.md.
void
gpu_set_line_width(float width)
{
	(void)width;
}

FsvRenderMode
gpu_render_mode(void)
{
	return g_render_mode;
}

void
gpu_set_landscape(int index)
{
	if (index < 0 || index >= FSN_LANDSCAPE_COUNT)
		index = FSN_LANDSCAPE_OFF; // defensive fallback -- an
		    // out-of-range index (a stale/hand-edited ~/.fsvrc token
		    // nvstore couldn't resolve, or a future caller's bug) draws
		    // nothing rather than reading fsn_landscapes[] out of bounds.
	g_landscape_index = index;
}

// ---- Landscape (fsn-mode Task A1): sky gradient + ground plane --------
//
// Drawn by gpu_scene_begin(), before main.cpp's geometry_draw() call.
// Two deliberate departures from the task brief's own sketch, both worth
// explaining since nothing else in the codebase does:
//
// 1. "vertex-colored gradient quad" isn't expressible through gpu_draw():
//    FsvVertex is {pos, normal} only, and scene.frag reads its fill
//    color from a *uniform* (gpu_set_color()), not a vertex attribute --
//    there is no per-vertex color channel anywhere in this contract.
//    Adding one would mean a second vertex format, a dedicated pipeline
//    and a new compiled shader pair (MSL + SPIR-V, Task 3.1's whole
//    offline toolchain) just for a two-triangle gradient. Instead the
//    sky is SKY_BANDS flat-colored horizontal strips, each one a
//    completely ordinary gpu_draw() call with a CPU-lerped color between
//    sky_top and sky_horizon -- the "smallest correct choice" the brief
//    itself invites for exactly this situation, at the cost of an
//    (imperceptible, at 32 bands) banded gradient instead of a smooth
//    one.
//
// 2. "depth-write off" is FSV_DEPTH_ALWAYS_NOWRITE (gpu.h), not a fixed
//    near-far NDC depth trick. An earlier version of this function parked
//    the sky at NDC z=0.999 (just under gpu_scene_end()'s 1.0 depth
//    clear) through an identity projection/modelview, reasoning that
//    every real draw would be "nearer" and win FSV_DEPTH_LESS. That
//    reasoning silently assumed *linear* depth: MapV/TreeV's near:far
//    ratio is 128:1 (camera.h's NEAR_TO_DISTANCE_RATIO *
//    FAR_TO_NEAR_RATIO), and glm_frustum_rh_zo's non-linear zero-to-one
//    depth compresses the far portion of the frustum into a thin band
//    just under NDC 1.0 -- so real geometry legitimately close to the far
//    clip plane could land *behind* 0.999 and lose the depth test to the
//    sky, which would incorrectly occlude it. FSV_DEPTH_ALWAYS_NOWRITE
//    (disables the depth test outright, so nothing is ever written) has
//    no such failure mode regardless of the projection's shape.
//
// The ground, unlike the sky, is real 3D geometry drawn with the actual
// camera matrices (restored right after the sky bands) and ordinary
// FSV_DEPTH_LESS test/write like any other opaque draw, so file/folder
// boxes correctly draw over it and it correctly occludes whatever's
// behind it. It is also mode-gated (see draw_landscape() below), unlike
// the sky: MapV/TreeV share a world z=0 "floor" convention the ground
// plane sits just under, but DiscV's camera is not oriented the same
// way (see setup_modelview_matrix()'s FSV_DISCV case, which applies a
// fixed axis-swapping rotation rather than reading camera->phi/theta at
// all) -- empirically, DiscV's camera looks straight down the world Z
// axis, so an unconditional ground plane there is not a thin strip near
// a horizon but a full-frame wall filling the entire view behind the
// disc for any real (non-tiny) directory, which a small fixture-only
// screenshot never triggers (the effect only appears once camera
// distance clears roughly 96 world units -- see the ground/near-far math
// in the task report). DiscV keeps the sky (a level, mode-agnostic
// backdrop) but never draws the ground.

namespace {

// Enough bands that a screenshot reads as a smooth gradient (see
// task-A1-report.md); each band costs one more gpu_draw() call, which is
// trivial next to a typical frame's node count.
constexpr int SKY_BANDS = 32;

// Arbitrary: FSV_DEPTH_ALWAYS_NOWRITE means the depth test is disabled
// for these draws, so this value affects neither occlusion nor z-fight
// risk. It only has to stay within this API's zero-to-one NDC depth
// range so the near/far clip stages (a separate pipeline stage from
// depth *testing*) don't discard the quad.
constexpr float SKY_NDC_DEPTH = 0.5f;

// Half-extent of the ground quad as a fraction of the frame's far clip
// distance, centered under the camera's eye. Sized so the quad's corners
// (half-extent x sqrt(2) away) stay just inside far_clip: with the
// pipelines now clipping against the near/far planes like GL always did
// (pipeline_for()'s enable_depth_clip comment), a fixed 100000-unit quad
// would get far-clipped into a visible "tent" horizon -- the far plane
// intersects each of the quad's two giant triangles in a straight 3D
// line, and the two projected lines meet in a peak. Kept just inside far
// instead: the quad's straight edges then sit ~0.7 x far_clip out, where
// the drop below the true horizon is atan(eye_height / (0.7 far)) --
// far_clip is 64x the camera-to-target distance (camera.h's
// NEAR_TO_DISTANCE_RATIO * FAR_TO_NEAR_RATIO) and eye height is at most
// 1x that same distance, so under a degree: not visible at any pose the
// app can reach.
constexpr float GROUND_EXTENT_FAR_FRAC = 0.68f;

// geometry.c's MapV layout puts the *bottom* of the root node's box at
// z=0 and stacks every directory upward from there (see
// geometry_mapv_node_z0(), src/geometry.c); world "up" is +z here (see
// g_base_modelview's own comment above). A ground quad at exactly z=0
// would sit in the same plane as that bottom face -- nudging it down by
// GROUND_Z_OFFSET avoids a z-fight with MapV's own base without being
// visually distinguishable at any of MapV's scales (mapv_leaf_height is
// 128; 6 units is under 5% of that). One shared ground plane for MapV
// and TreeV for now (see draw_landscape()'s mode gate); Task B3 is what
// scopes the whole landscape to FSV_FSN specifically.
// Also assumes the camera never dips below the ground plane and looks
// up through it: the ground's front face is wound to face +z (the
// direction every one of this app's camera positions actually views it
// from -- birdseye and oblique alike), so from below it would simply be
// back-face culled, which is arguably the more correct behavior for an
// opaque plane with no modeled thickness anyway.
constexpr float GROUND_Z_OFFSET = -6.0f;

} // namespace

static void
draw_landscape(int index)
{
	if (index < 0 || index >= FSN_LANDSCAPE_COUNT)
		return;
	const FsnLandscape &land = fsn_landscapes[index];

	// Ground is only meaningful in modes that share MapV's world z=0
	// "floor" convention -- see the block comment above. Every FsvMode
	// value is listed explicitly (no `default:`) so a future addition
	// (FSV_FSN, Task B1) has to make a deliberate choice here instead of
	// silently inheriting one; grep for this switch alongside the other
	// FsvMode switches the fsn-mode plan tracks.
	bool draw_ground;
	switch (globals.fsv_mode) {
		// FSN is the mode the landscape was built for in the first
		// place: its pedestals stand on world z == 0, the same floor
		// MapV and TreeV use, so the same ground quad (parked
		// GROUND_Z_OFFSET below it) sits under all three.
		case FSV_FSN:
		case FSV_MAPV:
		case FSV_TREEV:
		draw_ground = true;
		break;

		case FSV_DISCV:
		case FSV_SPLASH:
		case FSV_NONE:
		draw_ground = false;
		break;

		SWITCH_FAIL
	}

	// ---- Sky: SKY_BANDS horizontal strips, screen-space quads ---------
	mat4 saved_projection, saved_modelview;
	glm_mat4_copy(gpu_mat.projection, saved_projection);
	glm_mat4_copy(gpu_mat.modelview, saved_modelview);

	glm_mat4_identity(gpu_mat.projection);
	glm_mat4_identity(gpu_mat.modelview);
	gpu_upload_matrices();

	gpu_set_lighting(0);
	gpu_set_depth_test(FSV_DEPTH_ALWAYS_NOWRITE);
	for (int i = 0; i < SKY_BANDS; i++) {
		const float t0 = (float)i / (float)SKY_BANDS;       // top of band
		const float t1 = (float)(i + 1) / (float)SKY_BANDS; // bottom of band
		// NDC y+ is up (see this file's "Clip space" note at the top),
		// so t=0 (sky_top) is the top of the screen (y=+1).
		const float y_top = 1.0f - 2.0f * t0;
		const float y_bot = 1.0f - 2.0f * t1;

		float c[3];
		for (int k = 0; k < 3; k++)
			c[k] = land.sky_top[k] +
			    t0 * (land.sky_horizon[k] - land.sky_top[k]);
		gpu_set_color(c[0], c[1], c[2], 1.0f);

		// Counter-clockwise winding in NDC (Y-up) -- pipeline_for()
		// culls back faces (SDL_GPU_CULLMODE_BACK, front = CCW), same as
		// the GL frontend's defaults (see that function's own comment).
		const FsvVertex verts[4] = {
			{ { -1.0f, y_bot, SKY_NDC_DEPTH }, { 0.f, 0.f, 0.f } }, // bottom-left
			{ {  1.0f, y_bot, SKY_NDC_DEPTH }, { 0.f, 0.f, 0.f } }, // bottom-right
			{ {  1.0f, y_top, SKY_NDC_DEPTH }, { 0.f, 0.f, 0.f } }, // top-right
			{ { -1.0f, y_top, SKY_NDC_DEPTH }, { 0.f, 0.f, 0.f } }, // top-left
		};
		static const unsigned int idx[6] = { 0, 1, 2, 0, 2, 3 };
		gpu_draw(FSV_TRIANGLES, verts, 4, idx, 6);
	}

	// Restore the real camera matrices and the default depth test before
	// either drawing the ground (which needs both) or returning control
	// to geometry_draw() (which assumes gpu_scene_begin()'s
	// FSV_DEPTH_LESS baseline, same as every prior frame).
	glm_mat4_copy(saved_projection, gpu_mat.projection);
	glm_mat4_copy(saved_modelview, gpu_mat.modelview);
	gpu_upload_matrices();
	gpu_set_depth_test(FSV_DEPTH_LESS);

	if (!draw_ground)
		return;

	// ---- Ground: one large quad at world z = GROUND_Z_OFFSET ----------
	gpu_set_color(land.ground[0], land.ground[1], land.ground[2], 1.0f);
	// gpu_set_lighting(0) from the sky loop above is still in effect --
	// a flat plane would light uniformly across its single normal anyway.
	//
	// Center the quad under the camera's eye and size it to the frame's
	// far clip (GROUND_EXTENT_FAR_FRAC's comment above) so it always
	// covers the visible frustum without ever crossing the far plane.
	// The eye position comes from inverting the modelview just restored
	// above -- mode-agnostic, unlike camera_ground_position(), which
	// assumes MapVCamera's XYZvec target and so excludes TreeV (a ground
	// mode, see the switch at the top).
	mat4 view_inverse;
	vec4 world_origin = { 0.f, 0.f, 0.f, 1.f }, eye;
	glm_mat4_inv(gpu_mat.modelview, view_inverse);
	glm_mat4_mulv(view_inverse, world_origin, eye);
	const float e = GROUND_EXTENT_FAR_FRAC * (float)camera->far_clip;
	const float cx = eye[0], cy = eye[1];
	const float z = GROUND_Z_OFFSET;
	const FsvVertex ground_verts[4] = {
		{ { cx - e, cy - e, z }, { 0.f, 0.f, 1.f } },
		{ { cx + e, cy - e, z }, { 0.f, 0.f, 1.f } },
		{ { cx + e, cy + e, z }, { 0.f, 0.f, 1.f } },
		{ { cx - e, cy + e, z }, { 0.f, 0.f, 1.f } },
	};
	static const unsigned int ground_idx[6] = { 0, 1, 2, 0, 2, 3 };
	gpu_draw(FSV_TRIANGLES, ground_verts, 4, ground_idx, 6);
}

// ---- Overview mini-map (fsn-mode Task C1) ----------------------------
//
// A second render of the same FSN scene, from straight above, through an
// orthographic projection framing the whole landscape, into a small
// texture ImGui shows in a picture-in-picture window (src/sdl/
// ui_overview.cpp). Reference: the "overview" window in the top-right of
// 3060c037-069f-4715-a01e-c30e53e505a2.jpg.
//
// What is deliberately absent from it:
//   - the landscape sky/ground (draw_landscape()): the sky is a stack of
//     screen-space quads, which from a top-down camera would simply
//     cover the entire mini-map, and the ground plane is replaced by the
//     pass's own clear color -- one flat fill instead of a 100000-unit
//     quad, and the same green either way (see the clear in
//     gpu_scene_end()).
//   - node labels and the path text: geometry_draw(FALSE), exactly as
//     gpu_pick() does. Unreadable at this scale, and text_draw_straight()
//     bills a per-glyph quad for each one.
//   - the selection spotlight: skipped by geometry-fsn-draw.c itself, on
//     gpu_overview_pass(). See the comment there.
// What is present: pedestals, file boxes and wires -- the landscape's
// actual shape, which is the whole point of a mini-map -- plus the
// camera marker drawn below.

namespace {

// Grows the framed rectangle to the mini-map texture's aspect ratio,
// centered, so the landscape is never anisotropically squashed. Only
// ever grows: shrinking to fit would crop the landscape the caller just
// asked to see whole.
void
overview_fit_aspect(double *x0, double *x1, double *y0, double *y1)
{
	const double aspect = (double)FSN_OVERVIEW_WIDTH /
	    (double)FSN_OVERVIEW_HEIGHT;
	const double w = *x1 - *x0, h = *y1 - *y0;

	if (w < h * aspect) {
		const double cx = 0.5 * (*x0 + *x1), half = 0.5 * h * aspect;
		*x0 = cx - half;
		*x1 = cx + half;
	} else {
		const double cy = 0.5 * (*y0 + *y1), half = 0.5 * w / aspect;
		*y0 = cy - half;
		*y1 = cy + half;
	}
}

// Works out the ground rectangle and the camera height for this render,
// into g_overview_*. False means there is nothing to frame (no FSN
// layout), in which case nothing is drawn at all and the previous
// mini-map stays on screen.
//
// The landscape's own bounding box (plus margin) is the frame's baseline,
// but a camera flown outside it pulls the frame open toward its own ground
// position -- growing only the side(s) the camera is beyond, one more pad
// past it -- so the mini-map still reads as "stable map, moving marker"
// for a camera near or over the landscape, while a camera flown well
// clear of it gets a wider map instead of a marker pinned unreadably to
// the edge (TODO.md's "Overview mini-map (Task C1)" ticket). The growth
// is capped at FSN_OVERVIEW_MAX_GROWTH times the landscape's own larger
// dimension, so a camera flown arbitrarily far away doesn't shrink the
// landscape to a speck; past the cap, draw_overview_marker()'s
// pre-existing edge-clamp is the fallback, same as before this frame ever
// tracked the camera.
//
// "Camera near or over the landscape" is NOT FSN's own establishing shot:
// that pose looks down at the whole tree from behind and above, so its
// ground-projected eye point sits outside the pedestals' own bounding box
// by construction -- the very first frame of any FSN session already
// exercises this growth path, by a modest amount. That is intended, not a
// regression: it is the direct fix for this ticket's complaint (the
// marker pinned flush to the edge with its distance unreadable), and it
// was confirmed against the reference and accepted as the new baseline
// (2026-08-19) rather than chasing a "no growth at startup" deadband that
// would just reintroduce the old edge-pinning for the establishing shot.
// The true no-growth case is a camera that has flown to sit over or near
// the landscape itself (e.g. after a look-at or warp onto a pedestal).
//
// The pad used on the side being chased is NOT simply `margin` (below):
// draw_overview_marker()'s own edge-clamp inset is FSN_OVERVIEW_MARKER_
// FRAC * 0.5 * the *final*, post-aspect-fit x-extent, and on a
// sufficiently stretched frame that inset outgrows `margin` -- FSN's own
// establishing shot is the concrete case, whose aspect-fit stretch leaves
// an x-extent of roughly 3.15x the landscape's own dimension, an inset of
// ~0.087x versus an 0.08x margin. When the inset is the bigger of the
// two, it -- not this function's padding -- ends up deciding how far
// inside the edge the marker actually sits, silently overriding the
// growth computed here. `chase_pad` below is padded by whichever of the
// two is larger so the growth stays in charge and the marker lands a
// full, honest margin inside the frame, matching draw_overview_marker()'s
// own comment that its clamp is a rare fallback, not the normal source of
// the marker's placement.
//
// Circular in principle -- the inset is sized off the extent this very
// computation produces -- resolved with one extra trial layout rather
// than an analytic solve: lay out and aspect-fit the rect with
// `chase_pad` starting at `margin`, read the inset the *result* implies,
// and redo the chase (only; the static per-axis margin above is
// untouched) with that inset as `chase_pad` if it came out larger. A
// second pass can only grow `chase_pad` further, never shrink it, so one
// extra pass already lands within the same small, hard-capped multiple of
// `margin` that FSN_OVERVIEW_MAX_GROWTH bounds everything else to here --
// good enough to keep the clamp inert without an exact fixed point.
bool
overview_frame_scene(void)
{
	double x0, x1, y0, y1, height, margin, landscape_dim, growth_cap;
	double base_x0, base_x1, base_y0, base_y1, chase_pad;
	XYZvec cam_pos;
	int pass;

	if (fsn_layout_root() == nullptr)
		return false;

	fsn_layout_bounds(&x0, &x1, &y0, &y1);
	fsn_layout_extents(nullptr, nullptr, &height);
	if (!(x1 > x0) || !(y1 > y0))
		return false; // degenerate layout (should not happen: every
			      // pedestal has a real footprint)

	landscape_dim = MAX(x1 - x0, y1 - y0);
	margin = FSN_OVERVIEW_MARGIN * landscape_dim;
	base_x0 = x0 - margin;
	base_x1 = x1 + margin;
	base_y0 = y0 - margin;
	base_y1 = y1 + margin;

	// Chase the camera, per axis, only on the side it has actually left
	// the (margined) frame on -- never recenter the landscape needlessly
	// on an axis the camera hasn't left. `camera_ground_position()` is
	// the same eye-point derivation draw_overview_marker() uses for its
	// marker (src/camera.c).
	//
	// growth_cap bounds each axis's extent *here*, before overview_fit_
	// aspect() below stretches whichever axis is short to the mini-map's
	// fixed 512x320 texture aspect. On a diagonal-outside pose, that
	// aspect-fit step can itself push the capped axis up to roughly
	// FSN_OVERVIEW_WIDTH/FSN_OVERVIEW_HEIGHT (1.6x) past growth_cap --
	// bounded (aspect-fit only ever grows to a fixed ratio, never
	// unboundedly) and accepted, not fixed here: it is still a hard,
	// small multiple of growth_cap itself, not a new unbounded case.
	camera_ground_position(camera, &cam_pos);
	growth_cap = FSN_OVERVIEW_MAX_GROWTH * landscape_dim;

	chase_pad = margin;
	for (pass = 0; pass < 2; pass++) {
		double inset;

		x0 = base_x0;
		x1 = base_x1;
		y0 = base_y0;
		y1 = base_y1;

		if (cam_pos.x < x0)
			x0 = MAX(cam_pos.x - chase_pad, x1 - growth_cap);
		else if (cam_pos.x > x1)
			x1 = MIN(cam_pos.x + chase_pad, x0 + growth_cap);
		if (cam_pos.y < y0)
			y0 = MAX(cam_pos.y - chase_pad, y1 - growth_cap);
		else if (cam_pos.y > y1)
			y1 = MIN(cam_pos.y + chase_pad, y0 + growth_cap);

		overview_fit_aspect(&x0, &x1, &y0, &y1);

		// draw_overview_marker()'s own inset, for the extent this
		// pass just produced. If `chase_pad` already covers it,
		// this layout is the final one; otherwise grow `chase_pad`
		// to match and lay out once more.
		inset = FSN_OVERVIEW_MARKER_FRAC * 0.5 * (x1 - x0);
		if (inset <= chase_pad)
			break;
		chase_pad = inset;
	}

	g_overview_x0 = x0;
	g_overview_x1 = x1;
	g_overview_y0 = y0;
	g_overview_y1 = y1;

	// Straight above the tallest thing in the scene, looking down. An
	// orthographic projection makes the exact height irrelevant to what
	// the image looks like; all it has to do is keep every object
	// strictly between the near and far planes.
	g_overview_eye_height = (float)(height + FSN_OVERVIEW_HEADROOM);
	g_overview_far_clip = g_overview_eye_height +
	    (float)FSN_OVERVIEW_HEADROOM;
	return true;
}

} // namespace

// The top-down view matrix and orthographic projection for the rectangle
// overview_frame_scene() last computed. Called by gpu_scene_begin() in
// place of setup_projection_matrix()/setup_modelview_matrix().
//
// The view is a pure translation: with world +z up (see
// g_base_modelview), a camera at (cx, cy, H) looking straight down has
// eye axes that coincide with the world's -- right = +x, up = +y,
// backwards = +z -- so its rotation is the identity, and world +y ends up
// as "up" on the mini-map. Worth spelling out because the obvious
// alternative (mapping eye z to -world z) is a reflection: it would flip
// every triangle's winding and back-face cull the entire scene away.
static void
setup_overview_matrices(void)
{
	const float cx = (float)(0.5 * (g_overview_x0 + g_overview_x1));
	const float cy = (float)(0.5 * (g_overview_y0 + g_overview_y1));
	const float half_w = (float)(0.5 * (g_overview_x1 - g_overview_x0));
	const float half_h = (float)(0.5 * (g_overview_y1 - g_overview_y0));
	vec3 eye = { -cx, -cy, -g_overview_eye_height };

	// Near clip well above the geometry: everything drawn lives between
	// world z = 0 and eye_height - FSN_OVERVIEW_HEADROOM, i.e. eye z in
	// [-eye_height, -FSN_OVERVIEW_HEADROOM].
	glm_ortho_rh_zo(-half_w, half_w, -half_h, half_h, 1.0f,
	    g_overview_far_clip, gpu_mat.projection);

	glm_mat4_identity(gpu_mat.modelview);
	glm_translate(gpu_mat.modelview, eye);
}

// The camera marker: a flat arrowhead on the ground at the camera's own
// ground position, pointing the way it is looking.
//
// Position comes from camera_ground_position() (src/camera.c), which
// solves setup_modelview_matrix()'s FSV_FSN/FSV_MAPV eye-point transform
// for the same live camera (FSN reuses MapV's camera storage -- see
// camera.c's FSN_CAMERA_* note) -- overview_frame_scene() above uses the
// identical call to chase this same camera when framing the mini-map.
// Heading comes straight from camera->theta: the camera looks back along
// -(cos(theta), sin(theta)), which is the arrow's direction.
//
// Drawn with the depth test off (FSV_DEPTH_ALWAYS_NOWRITE) so a pedestal
// the camera happens to be standing over cannot hide it, and after
// geometry_draw() so it also paints over anything already there.
static void
draw_overview_marker(void)
{
	const double theta = RAD(camera->theta);
	// Ground-projected forward direction: from the camera towards its
	// target (see above).
	const double fx = -cos(theta), fy = -sin(theta);
	// Left-hand perpendicular, for the two base corners.
	const double lx = -fy, ly = fx;

	const double size = FSN_OVERVIEW_MARKER_FRAC *
	    0.5 * (g_overview_x1 - g_overview_x0);

	XYZvec cam_ground_pos;
	camera_ground_position(camera, &cam_ground_pos);
	double cx = cam_ground_pos.x;
	double cy = cam_ground_pos.y;

	// Clamped into the framed rectangle (inset by the marker's own size,
	// so it is never half off the edge): a camera pulled far back sits
	// outside the landscape it is looking at, and a marker that silently
	// vanished off the edge would read as "the overview is broken"
	// rather than "you are standing outside the tree". Genuinely a
	// fallback, not the normal source of the marker's placement:
	// overview_frame_scene()'s `chase_pad` pads the chased side by at
	// least this same inset (computed from the same final extent), so
	// while the camera is within FSN_OVERVIEW_MAX_GROWTH the position
	// above already lands inside these bounds and this CLAMP is a
	// no-op. It only actually moves the marker once growth has hit that
	// cap and the frame can no longer follow the camera out.
	cx = CLAMP(cx, g_overview_x0 + size, g_overview_x1 - size);
	cy = CLAMP(cy, g_overview_y0 + size, g_overview_y1 - size);

	// Counter-clockwise seen from above (the direction this pass views
	// it from), matching pipeline_for()'s front-face winding.
	const float z = 0.0f;
	const FsvVertex verts[3] = {
		{ { (float)(cx + fx * size), (float)(cy + fy * size), z },
		  { 0.f, 0.f, 1.f } },
		{ { (float)(cx - fx * size * 0.6 + lx * size * 0.6),
		    (float)(cy - fy * size * 0.6 + ly * size * 0.6), z },
		  { 0.f, 0.f, 1.f } },
		{ { (float)(cx - fx * size * 0.6 - lx * size * 0.6),
		    (float)(cy - fy * size * 0.6 - ly * size * 0.6), z },
		  { 0.f, 0.f, 1.f } },
	};

	gpu_set_depth_test(FSV_DEPTH_ALWAYS_NOWRITE);
	gpu_set_lighting(0);
	gpu_set_color(FSN_OVERVIEW_MARKER_R, FSN_OVERVIEW_MARKER_G,
	    FSN_OVERVIEW_MARKER_B, 1.0f);
	gpu_draw(FSV_TRIANGLES, verts, 3, nullptr, 0);
	gpu_set_depth_test(FSV_DEPTH_LESS);
}

// Creates the mini-map texture and its depth buffer on first use. Both
// are fixed-size (FSN_OVERVIEW_WIDTH/HEIGHT, see fsn-style.h), so this is
// a one-time cost.
static bool
ensure_overview_targets(void)
{
	if (g_overview_texture != nullptr && g_overview_depth != nullptr)
		return true;

	SDL_GPUTextureCreateInfo info = {};
	info.type = SDL_GPU_TEXTURETYPE_2D;
	// Same format as gpu_pick()/--screenshot's targets, so this reuses
	// pipeline_for()'s existing target-index-1 pipelines rather than
	// adding a third color-target format. SAMPLER on top of
	// COLOR_TARGET: ImGui's backend binds this texture to its fragment
	// shader (imgui_impl_sdlgpu3.cpp's SDL_BindGPUFragmentSamplers()).
	info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
	    SDL_GPU_TEXTUREUSAGE_SAMPLER;
	info.width = FSN_OVERVIEW_WIDTH;
	info.height = FSN_OVERVIEW_HEIGHT;
	info.layer_count_or_depth = 1;
	info.num_levels = 1;
	info.sample_count = SDL_GPU_SAMPLECOUNT_1;

	if (g_overview_texture == nullptr) {
		g_overview_texture = SDL_CreateGPUTexture(g_device, &info);
		if (g_overview_texture == nullptr) {
			SDL_Log("gpu: overview texture creation failed: %s",
			    SDL_GetError());
			return false;
		}
	}

	SDL_GPUTextureCreateInfo depth_info = {};
	depth_info.type = SDL_GPU_TEXTURETYPE_2D;
	depth_info.format = g_depth_format;
	depth_info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
	depth_info.width = FSN_OVERVIEW_WIDTH;
	depth_info.height = FSN_OVERVIEW_HEIGHT;
	depth_info.layer_count_or_depth = 1;
	depth_info.num_levels = 1;
	depth_info.sample_count = SDL_GPU_SAMPLECOUNT_1;

	g_overview_depth = SDL_CreateGPUTexture(g_device, &depth_info);
	if (g_overview_depth == nullptr) {
		SDL_Log("gpu: overview depth texture creation failed: %s",
		    SDL_GetError());
		return false;
	}

	g_overview_width = FSN_OVERVIEW_WIDTH;
	g_overview_height = FSN_OVERVIEW_HEIGHT;
	return true;
}

int
gpu_overview_pass(void)
{
	return g_overview_frame ? 1 : 0;
}

bool
gpu_overview_render(void)
{
	// Runs inside the caller's frame, on the frame's own command buffer
	// and *before* the visible scene pass, so that ImGui's later pass in
	// that same buffer samples a texture this frame already filled. It
	// is therefore not like gpu_pick(), which owns its command buffer
	// precisely because it runs outside any frame -- this one requires
	// one to be open, and refuses to start a second scene inside the
	// first (g_recording).
	if (!g_ready || g_cmd == nullptr || g_recording)
		return false;
	if (globals.fsv_mode != FSV_FSN)
		return false;
	if (!overview_frame_scene())
		return false;
	if (!ensure_overview_targets())
		return false;

	// gpu_scene_begin() reads this flag three times: to pick this
	// texture as the color target, to build the top-down matrices
	// instead of the live camera's, and to skip the landscape.
	g_overview_frame = true;
	gpu_scene_begin();
	if (!g_recording) {
		g_overview_frame = false;
		return false; // gpu_scene_begin() declined the frame
	}
	// FALSE: no labels, no path text -- see this section's header.
	geometry_draw(FALSE);
	draw_overview_marker();
	gpu_scene_end();
	g_overview_frame = false;

	g_overview_framed = true;
	return true;
}

SDL_GPUTexture *
gpu_overview_texture(void)
{
	return g_overview_framed ? g_overview_texture : nullptr;
}

void
gpu_overview_frame_rect(double *x0, double *x1, double *y0, double *y1)
{
	if (x0 != nullptr)
		*x0 = g_overview_x0;
	if (x1 != nullptr)
		*x1 = g_overview_x1;
	if (y0 != nullptr)
		*y0 = g_overview_y0;
	if (y1 != nullptr)
		*y1 = g_overview_y1;
}

void
gpu_init(void *sdl_window)
{
	g_window = (SDL_Window *)sdl_window;

	g_device = SDL_CreateGPUDevice(
	    SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_SPIRV,
	    /* debug */ true, nullptr);
	if (g_device == nullptr) {
		SDL_Log("gpu: SDL_CreateGPUDevice failed: %s", SDL_GetError());
		return;
	}
	SDL_Log("gpu: driver %s, shader formats 0x%x",
	    SDL_GetGPUDeviceDriver(g_device),
	    (unsigned)SDL_GetGPUShaderFormats(g_device));

	if (!SDL_ClaimWindowForGPUDevice(g_device, g_window)) {
		SDL_Log("gpu: SDL_ClaimWindowForGPUDevice failed: %s",
		    SDL_GetError());
		return;
	}

	if (!choose_depth_format())
		return;

	// MSL first (native on Metal), SPIR-V second. Entry point names per
	// Task 3.1: glslang keeps "main" in SPIR-V, SPIRV-Cross renames it
	// to "main0" in MSL because Metal reserves `main`.
	const ShaderBlob vert_blobs[] = {
		{ SDL_GPU_SHADERFORMAT_MSL, "main0", scene_vert_msl,
		  sizeof scene_vert_msl, "scene.vert.msl" },
		{ SDL_GPU_SHADERFORMAT_SPIRV, "main", scene_vert_spv,
		  sizeof scene_vert_spv, "scene.vert.spv" },
	};
	const ShaderBlob frag_blobs[] = {
		{ SDL_GPU_SHADERFORMAT_MSL, "main0", scene_frag_msl,
		  sizeof scene_frag_msl, "scene.frag.msl" },
		{ SDL_GPU_SHADERFORMAT_SPIRV, "main", scene_frag_spv,
		  sizeof scene_frag_spv, "scene.frag.spv" },
	};

	g_vert_shader = create_shader(SDL_GPU_SHADERSTAGE_VERTEX,
	    vert_blobs, (int)SDL_arraysize(vert_blobs), /* uniform buffers */ 1);
	g_frag_shader = create_shader(SDL_GPU_SHADERSTAGE_FRAGMENT,
	    frag_blobs, (int)SDL_arraysize(frag_blobs), /* uniform buffers */ 1);
	if (g_vert_shader == nullptr || g_frag_shader == nullptr) {
		SDL_Log("gpu: no usable scene shader");
		return;
	}

	// Build the two pipelines every frame uses up front, so a broken
	// pipeline state is a startup failure rather than a mid-frame one.
	// The rest (cursor depth tests, picking target) are created on
	// first use by pipeline_for().
	if (pipeline_for(SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, FSV_DEPTH_LESS, 0) ==
	        nullptr ||
	    pipeline_for(SDL_GPU_PRIMITIVETYPE_LINELIST, FSV_DEPTH_LESS, 0) ==
	        nullptr)
		return; // g_ready stays false; main.cpp exits
	SDL_Log("gpu: scene pipelines created");

	// Initial matrix state, from ogl_init() (src/ogl.c:182): a
	// right-handed frame with +z straight up and the camera at the
	// origin looking down -x.
	glm_mat4_identity(g_base_modelview);
	glm_rotate_x(g_base_modelview, -M_PI_2, g_base_modelview);
	glm_rotate_z(g_base_modelview, -M_PI_2, g_base_modelview);
	glm_mat4_copy(g_base_modelview, gpu_mat.modelview);
	glm_mat4_identity(gpu_mat.projection);

	// Lighting constants, from ogl_init(): the GL frontend uploaded
	// only the first component of each {ambient,diffuse,specular}
	// color and let the shader multiply by white.
	g_frag_ubo.ambient = 0.2f;
	g_frag_ubo.diffuse = 0.6f;
	g_frag_ubo.specular = 0.3f;
	g_vert_ubo.light_pos[0] = 0.2f;
	g_vert_ubo.light_pos[1] = 0.0f;
	g_vert_ubo.light_pos[2] = 1.0f;
	g_vert_ubo.light_pos[3] = 0.0f; // w == 0: light at infinity
	gpu_set_color(1.0f, 1.0f, 1.0f, 1.0f);
	gpu_set_lighting(1); // ogl_init() calls ogl_enable_lightning()
	gpu_upload_matrices();

	g_ready = true;
}

bool
gpu_ready(void)
{
	return g_ready;
}

void
gpu_shutdown(void)
{
	if (g_device == nullptr)
		return;

	for (int p = 0; p < NUM_PRIMS; p++)
		for (int d = 0; d < NUM_DEPTH_TESTS; d++)
			for (int t = 0; t < NUM_TARGETS; t++) {
				if (g_pipelines[p][d][t] == nullptr)
					continue;
				SDL_ReleaseGPUGraphicsPipeline(g_device,
				    g_pipelines[p][d][t]);
				g_pipelines[p][d][t] = nullptr;
			}
	if (g_vert_shader != nullptr)
		SDL_ReleaseGPUShader(g_device, g_vert_shader);
	if (g_frag_shader != nullptr)
		SDL_ReleaseGPUShader(g_device, g_frag_shader);
	if (g_depth_texture != nullptr)
		SDL_ReleaseGPUTexture(g_device, g_depth_texture);
	if (g_vertex_buffer != nullptr)
		SDL_ReleaseGPUBuffer(g_device, g_vertex_buffer);
	if (g_index_buffer != nullptr)
		SDL_ReleaseGPUBuffer(g_device, g_index_buffer);
	if (g_transfer != nullptr)
		SDL_ReleaseGPUTransferBuffer(g_device, g_transfer);
	g_vert_shader = nullptr;
	g_frag_shader = nullptr;
	g_depth_texture = nullptr;
	g_vertex_buffer = nullptr;
	g_index_buffer = nullptr;
	g_transfer = nullptr;
	g_vertex_capacity = g_index_capacity = g_transfer_capacity = 0;

	for (int t = 0; t < NUM_TARGETS; t++) {
		if (g_text_pipelines[t] == nullptr)
			continue;
		SDL_ReleaseGPUGraphicsPipeline(g_device, g_text_pipelines[t]);
		g_text_pipelines[t] = nullptr;
	}
	if (g_text_vert_shader != nullptr)
		SDL_ReleaseGPUShader(g_device, g_text_vert_shader);
	if (g_text_frag_shader != nullptr)
		SDL_ReleaseGPUShader(g_device, g_text_frag_shader);
	if (g_text_sampler != nullptr)
		SDL_ReleaseGPUSampler(g_device, g_text_sampler);
	if (g_text_texture != nullptr)
		SDL_ReleaseGPUTexture(g_device, g_text_texture);
	if (g_text_vertex_buffer != nullptr)
		SDL_ReleaseGPUBuffer(g_device, g_text_vertex_buffer);
	if (g_text_index_buffer != nullptr)
		SDL_ReleaseGPUBuffer(g_device, g_text_index_buffer);
	if (g_text_transfer != nullptr)
		SDL_ReleaseGPUTransferBuffer(g_device, g_text_transfer);
	g_text_vert_shader = nullptr;
	g_text_frag_shader = nullptr;
	g_text_sampler = nullptr;
	g_text_texture = nullptr;
	g_text_vertex_buffer = nullptr;
	g_text_index_buffer = nullptr;
	g_text_transfer = nullptr;
	g_text_vertex_capacity = g_text_index_capacity =
	    g_text_transfer_capacity = 0;

	// fsn-mode Task C1: the overview mini-map's cached targets (the only
	// offscreen textures in this file that outlive their own render --
	// see their declaration).
	if (g_overview_texture != nullptr)
		SDL_ReleaseGPUTexture(g_device, g_overview_texture);
	if (g_overview_depth != nullptr)
		SDL_ReleaseGPUTexture(g_device, g_overview_depth);
	g_overview_texture = nullptr;
	g_overview_depth = nullptr;
	g_overview_width = g_overview_height = 0;
	g_overview_framed = false;

	g_ready = false;

	SDL_ReleaseWindowFromGPUDevice(g_device, g_window);
	SDL_DestroyGPUDevice(g_device);
	g_device = nullptr;
	g_window = nullptr;
}

SDL_GPUDevice *
gpu_device(void)
{
	return g_device;
}

SDL_GPUCommandBuffer *
gpu_frame_begin(void)
{
	g_swapchain = nullptr;
	g_swapchain_width = g_swapchain_height = 0;
	g_recording = false;

	g_cmd = SDL_AcquireGPUCommandBuffer(g_device);
	if (g_cmd == nullptr) {
		SDL_Log("gpu: SDL_AcquireGPUCommandBuffer failed: %s",
		    SDL_GetError());
		return nullptr;
	}

	if (!SDL_WaitAndAcquireGPUSwapchainTexture(g_cmd, g_window,
	        &g_swapchain, &g_swapchain_width, &g_swapchain_height))
		SDL_Log("gpu: swapchain acquire failed: %s", SDL_GetError());

	if (g_swapchain != nullptr &&
	    !ensure_depth_texture(g_swapchain_width, g_swapchain_height))
		g_swapchain = nullptr; // nothing can be drawn this frame

	return g_cmd;
}

SDL_GPUTexture *
gpu_frame_swapchain_texture(void)
{
	return g_swapchain;
}

void
gpu_frame_end(void)
{
	if (g_cmd == nullptr)
		return;
	SDL_SubmitGPUCommandBuffer(g_cmd);
	g_cmd = nullptr;
	g_swapchain = nullptr;
}

void
gpu_scene_begin(void)
{
	// fsn-mode Task C1's overview mini-map is checked first and does not
	// consult g_capture_texture at all: a --record frame has one of its
	// own (in the swapchain's format, target index 0), and an overview
	// render nested inside such a frame must still go to its own
	// R8G8B8A8 texture with the matching pipelines. See the
	// g_overview_texture declaration.
	if (g_overview_frame) {
		g_color_target = g_overview_texture;
		g_target_index = 1; // R8G8B8A8, as for pick/--screenshot
	} else {
		g_color_target = g_capture_texture != nullptr ? g_capture_texture
							      : g_swapchain;
		g_target_index = g_capture_texture == nullptr ? 0
		    : g_recording_frame ? 0 /* matches swapchain format -- see g_recording_frame */
					 : 1;
	}
	if (g_cmd == nullptr || g_color_target == nullptr || !g_ready)
		return;

	// Same three calls the GTK frontend's render() made per frame
	// (src/ogl.c:432) -- except for the overview, which frames the whole
	// landscape from straight above instead of following the live camera
	// (setup_overview_matrices(), fsn-mode Task C1).
	if (g_overview_frame)
		setup_overview_matrices();
	else {
		setup_projection_matrix();
		setup_modelview_matrix();
	}
	gpu_upload_matrices();

	// Reset per-frame draw state. The vectors keep their capacity, so
	// steady-state frames do no allocation at all.
	g_vertices.clear();
	g_indices.clear();
	g_draws.clear();
	g_text_vertices.clear();
	g_text_indices.clear();
	g_text_draws.clear();
	g_depth_test = FSV_DEPTH_LESS;
	g_recording = true;

	// fsn-mode Task A1: sky gradient + ground plane, drawn before the
	// caller's geometry_draw() so they sit behind the real scene (see
	// "Landscape" above draw_landscape()'s own definition). g_render_mode
	// is already correct here -- gpu_pick() sets FSV_RENDER_SELECT
	// *before* calling gpu_scene_begin() -- so this is where the select
	// pass is excluded: the sky and ground must not be pickable (a click
	// on empty sky has to resolve to node id 0, matching an empty
	// background today), and neither one paints an id color at all.
	//
	// The overview pass skips it too (fsn-mode Task C1): the sky is a
	// stack of screen-space quads, which under a top-down camera would
	// cover the mini-map completely, and its ground plane is replaced by
	// the pass's clear color -- see gpu_scene_end().
	if (g_render_mode == FSV_RENDER_NORMAL && !g_overview_frame)
		draw_landscape(g_landscape_index);
}

void
gpu_draw(FsvTopology topology, const FsvVertex *verts, int nverts,
    const unsigned int *indices, int nindices)
{
	if (!g_recording || verts == nullptr || nverts <= 0)
		return;
	if (indices != nullptr && nindices <= 0)
		return;

	const Sint32 vertex_offset = (Sint32)g_vertices.size();
	const Uint32 first_index = (Uint32)g_indices.size();

	append_indices(topology, nverts, indices, nindices);
	const Uint32 num_indices = (Uint32)g_indices.size() - first_index;
	if (num_indices == 0)
		return; // degenerate batch (e.g. a 1-vertex "line")

	g_vertices.insert(g_vertices.end(), verts, verts + nverts);

	const bool is_line = topology == FSV_LINES ||
	    topology == FSV_LINE_STRIP || topology == FSV_LINE_LOOP;
	SDL_GPUGraphicsPipeline *pipeline = pipeline_for(
	    is_line ? SDL_GPU_PRIMITIVETYPE_LINELIST
	            : SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
	    g_depth_test, g_target_index);
	if (pipeline == nullptr) {
		g_indices.resize(first_index);
		return;
	}

	DrawCmd cmd;
	cmd.pipeline = pipeline;
	cmd.first_index = first_index;
	cmd.num_indices = num_indices;
	cmd.vertex_offset = vertex_offset;
	cmd.vert_ubo = g_vert_ubo;
	cmd.frag_ubo = g_frag_ubo;
	g_draws.push_back(cmd);
}

void
gpu_scene_end(void)
{
	if (!g_recording)
		return;
	g_recording = false;

	const bool have_geometry =
	    !g_draws.empty() && upload_frame_geometry();
	const bool have_text =
	    !g_text_draws.empty() && upload_frame_text_geometry();

	SDL_GPUColorTargetInfo color_target = {};
	color_target.texture = g_color_target;
	// src/ogl.c cleared to transparent black; the scene is opaque and
	// ImGui draws on top of it, so the alpha only matters for the
	// window background. Kept as Task 2.2's dark slate so an empty
	// scene is still visibly "the app running" rather than a black
	// screen indistinguishable from a hang.
	//
	// The id-color pass (gpu_pick(), Task 4.2) is the one exception:
	// ogl_select_modern() cleared to (0,0,0,0) so that a pixel nothing
	// draws over decodes as node id 0 ("nothing there") -- the dark
	// slate's non-zero bytes would otherwise read back as a bogus id
	// for e.g. a click on empty sky. gpu_pick() sets g_render_mode
	// before this call and restores it right after.
	//
	// The overview mini-map (fsn-mode Task C1) is the other exception:
	// its clear IS its ground. A top-down camera sees nothing of the sky
	// and nothing of the ground plane except its color, so
	// draw_landscape() is skipped for it entirely (see gpu_scene_begin())
	// and the ground arrives here instead, as one flat fill. The palette
	// is pinned to the "night" preset rather than following the Display
	// menu: that is what this task's reference screenshot's inset shows
	// (a flat green field), and night's ground is the same green as
	// classic's -- only its sky, which the overview never shows, differs.
	const FsnLandscape &overview_land =
	    fsn_landscapes[FSN_LANDSCAPE_NIGHT];
	color_target.clear_color = g_render_mode == FSV_RENDER_SELECT
	    ? SDL_FColor{ 0.0f, 0.0f, 0.0f, 0.0f }
	    : g_overview_frame
	    ? SDL_FColor{ overview_land.ground[0], overview_land.ground[1],
			  overview_land.ground[2], 1.0f }
	    : SDL_FColor{ 0.08f, 0.10f, 0.12f, 1.0f };
	color_target.load_op = SDL_GPU_LOADOP_CLEAR;
	color_target.store_op = SDL_GPU_STOREOP_STORE;

	SDL_GPUDepthStencilTargetInfo depth_target = {};
	// The overview renders at its own fixed resolution, so it carries
	// its own depth buffer: SDL_GPU requires every attachment in a pass
	// to be the same size, and ensure_depth_texture()'s single cached
	// texture is sized to the window.
	depth_target.texture = g_overview_frame ? g_overview_depth
						: g_depth_texture;
	depth_target.clear_depth = 1.0f;
	depth_target.load_op = SDL_GPU_LOADOP_CLEAR;
	// Nothing samples the depth buffer after the pass.
	depth_target.store_op = SDL_GPU_STOREOP_DONT_CARE;
	depth_target.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
	depth_target.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
	depth_target.cycle = true;

	SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(g_cmd, &color_target,
	    1, &depth_target);
	if (pass == nullptr) {
		SDL_Log("gpu: SDL_BeginGPURenderPass failed: %s",
		    SDL_GetError());
		return;
	}
	if (have_geometry)
		replay_draws(pass);
	if (have_text)
		replay_text_draws(pass);
	SDL_EndGPURenderPass(pass);
}

// ---- Text rendering (public API, src/tmaptext.c via src/gpu.h) -------

// Uploads the glyph atlas and builds the text shaders/pipelines. Called
// once, from text_init() (itself called once, from main.cpp, after
// gpu_init() has succeeded -- the device has to exist first).
void
gpu_text_init(const unsigned char *pixels, int width, int height)
{
	if (g_device == nullptr)
		return;

	const ShaderBlob vert_blobs[] = {
		{ SDL_GPU_SHADERFORMAT_MSL, "main0", text_vert_msl,
		  sizeof text_vert_msl, "text.vert.msl" },
		{ SDL_GPU_SHADERFORMAT_SPIRV, "main", text_vert_spv,
		  sizeof text_vert_spv, "text.vert.spv" },
	};
	const ShaderBlob frag_blobs[] = {
		{ SDL_GPU_SHADERFORMAT_MSL, "main0", text_frag_msl,
		  sizeof text_frag_msl, "text.frag.msl" },
		{ SDL_GPU_SHADERFORMAT_SPIRV, "main", text_frag_spv,
		  sizeof text_frag_spv, "text.frag.spv" },
	};
	g_text_vert_shader = create_shader(SDL_GPU_SHADERSTAGE_VERTEX,
	    vert_blobs, (int)SDL_arraysize(vert_blobs), /* uniform buffers */ 1);
	// text.frag declares one sampler2D (set=2, binding=0, the glyph
	// atlas) -- the resource count SDL_CreateGPUShader needs has to
	// match what the shader actually binds, or the pipeline silently
	// draws nothing (this was Task 3.4's first bug: create_shader()
	// used to hardcode num_samplers=0 for every shader, which is
	// correct for the scene shaders but wrong here).
	g_text_frag_shader = create_shader(SDL_GPU_SHADERSTAGE_FRAGMENT,
	    frag_blobs, (int)SDL_arraysize(frag_blobs), /* uniform buffers */ 1,
	    /* samplers */ 1);
	if (g_text_vert_shader == nullptr || g_text_frag_shader == nullptr) {
		SDL_Log("gpu: no usable text shader");
		return;
	}

	// The glyph atlas: a single-channel bitmap, sampled as alpha by
	// text.frag's `alpha.r`. R8_UNORM is the SDL_GPU equivalent of the
	// old GL code's GL_RED texture.
	SDL_GPUTextureCreateInfo tex_info = {};
	tex_info.type = SDL_GPU_TEXTURETYPE_2D;
	tex_info.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
	tex_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
	tex_info.width = (Uint32)width;
	tex_info.height = (Uint32)height;
	tex_info.layer_count_or_depth = 1;
	tex_info.num_levels = 1;
	tex_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
	g_text_texture = SDL_CreateGPUTexture(g_device, &tex_info);
	if (g_text_texture == nullptr) {
		SDL_Log("gpu: SDL_CreateGPUTexture (text atlas) failed: %s",
		    SDL_GetError());
		return;
	}

	// Linear filtering, clamp-to-edge. Simpler than the old GL sampler,
	// which mixed GL_LINEAR_MIPMAP_LINEAR minification with GL_NEAREST
	// magnification: this atlas has one mip level, so there is no
	// minification filter left to choose -- see docs/PORTING.md Task 3.4.
	SDL_GPUSamplerCreateInfo sampler_info = {};
	sampler_info.min_filter = SDL_GPU_FILTER_LINEAR;
	sampler_info.mag_filter = SDL_GPU_FILTER_LINEAR;
	sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
	sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	g_text_sampler = SDL_CreateGPUSampler(g_device, &sampler_info);
	if (g_text_sampler == nullptr) {
		SDL_Log("gpu: SDL_CreateGPUSampler (text atlas) failed: %s",
		    SDL_GetError());
		return;
	}

	// One-off upload on its own command buffer: the atlas never changes
	// after this, so it does not belong in the per-frame recording.
	const Uint32 bytes = (Uint32)width * (Uint32)height;
	SDL_GPUTransferBufferCreateInfo transfer_info = {};
	transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transfer_info.size = bytes;
	SDL_GPUTransferBuffer *upload =
	    SDL_CreateGPUTransferBuffer(g_device, &transfer_info);
	if (upload == nullptr) {
		SDL_Log("gpu: text atlas transfer buffer failed: %s",
		    SDL_GetError());
		return;
	}
	void *mapped = SDL_MapGPUTransferBuffer(g_device, upload, false);
	if (mapped == nullptr) {
		SDL_Log("gpu: text atlas map failed: %s", SDL_GetError());
		SDL_ReleaseGPUTransferBuffer(g_device, upload);
		return;
	}
	memcpy(mapped, pixels, bytes);
	SDL_UnmapGPUTransferBuffer(g_device, upload);

	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g_device);
	if (cmd != nullptr) {
		SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(cmd);
		SDL_GPUTextureTransferInfo source = {};
		source.transfer_buffer = upload;
		source.pixels_per_row = (Uint32)width;
		source.rows_per_layer = (Uint32)height;
		SDL_GPUTextureRegion destination = {};
		destination.texture = g_text_texture;
		destination.w = (Uint32)width;
		destination.h = (Uint32)height;
		destination.d = 1;
		SDL_UploadToGPUTexture(copy_pass, &source, &destination, false);
		SDL_EndGPUCopyPass(copy_pass);
		SDL_SubmitGPUCommandBuffer(cmd);
	} else
		SDL_Log("gpu: text atlas upload command buffer failed: %s",
		    SDL_GetError());
	SDL_ReleaseGPUTransferBuffer(g_device, upload);

	// Build both pipeline variants up front (swapchain + capture
	// formats), so a broken text pipeline is a startup failure, exactly
	// like gpu_init()'s scene pipelines.
	text_pipeline_for(0);
	text_pipeline_for(1);
}

// No-ops by design: text_pre()/text_post()'s old GL_BLEND/
// GL_POLYGON_OFFSET_FILL toggles and texture bind/unbind are all baked
// into the text pipeline (see text_pipeline_for()) or bound per-draw
// (see replay_text_draws()) on this backend. The GL compat shim still
// does the real state dance -- see src/ogl-gpu-compat.c.
void
gpu_text_begin(void)
{
}

void
gpu_text_end(void)
{
}

void
gpu_text_draw(const FsvTextVertex *verts, int nverts,
    const unsigned int *indices, int nindices)
{
	if (!g_recording || verts == nullptr || nverts <= 0 ||
	    indices == nullptr || nindices <= 0)
		return;

	const Sint32 vertex_offset = (Sint32)g_text_vertices.size();
	const Uint32 first_index = (Uint32)g_text_indices.size();

	g_text_indices.insert(g_text_indices.end(), indices, indices + nindices);
	g_text_vertices.insert(g_text_vertices.end(), verts, verts + nverts);

	TextDrawCmd cmd;
	cmd.first_index = first_index;
	cmd.num_indices = (Uint32)nindices;
	cmd.vertex_offset = vertex_offset;
	cmd.vert_ubo = g_text_vert_ubo;
	cmd.frag_ubo = g_text_frag_ubo;
	g_text_draws.push_back(cmd);
}

void
gpu_text_set_color(float r, float g, float b)
{
	g_text_frag_ubo.color[0] = r;
	g_text_frag_ubo.color[1] = g;
	g_text_frag_ubo.color[2] = b;
}

void
gpu_text_upload_mvp(const float *mvp)
{
	memcpy(g_text_vert_ubo.mvp, mvp, sizeof g_text_vert_ubo.mvp);
}

// Port of ogl_select_modern() (src/ogl.c:456): renders geometry.c's id-color
// pass into a private off-screen target and reads back the one texel under
// the cursor. `x`/`y` are already in viewport pixels (input.cpp's
// pixel_scale()), top-left origin -- see the Y-flip note below.
//
// Own command buffer, entirely outside any swapchain frame. This matters
// because gpu_scene_begin()/gpu_scene_end() are keyed off module-global state
// (g_cmd, g_color_target, g_target_index, g_recording, the g_vertices/
// g_indices/g_draws arenas) that the *visible* frame also uses --
// interleaving the two would corrupt whichever one runs second. It is safe
// here because input_handle_event() (this function's only caller, via
// node_at_cursor()) always runs inside main.cpp's SDL_PollEvent() loop,
// strictly before that iteration's own gpu_frame_begin()/submit_frame():
// see the loop in src/sdl/main.cpp. g_cmd is therefore always null on entry;
// the check below turns a violation of that invariant into a logged no-op
// pick rather than stomping the in-flight frame.
//
// Shared body of gpu_pick()/gpu_pick_window() (gpu.h): one id-color
// render, one readback of the GPU_PICK_WINDOW-square neighborhood
// around (x, y), clamped at the viewport edges. `ids` always holds
// GPU_PICK_WINDOW * GPU_PICK_WINDOW entries and is zero-filled up
// front, so every early-out leaves it in the documented "nothing
// there" state.
static unsigned int
pick_impl(int x, int y, unsigned int *ids)
{
	const int R = GPU_PICK_WINDOW / 2;

	memset(ids, 0, GPU_PICK_WINDOW * GPU_PICK_WINDOW * sizeof(*ids));

	if (!g_ready || g_window == nullptr)
		return 0;
	if (g_cmd != nullptr || g_recording) {
		SDL_Log("gpu: gpu_pick() called while a scene frame is in "
		    "flight; ignoring pick");
		return 0;
	}

	int width = 0, height = 0;
	SDL_GetWindowSizeInPixels(g_window, &width, &height);
	if (width <= 0 || height <= 0)
		return 0;
	// Matches node_at_cursor()'s only other guard (viewport_node_for_id()'s
	// bounds check on the id, not the pixel) -- a pixel outside the
	// current swapchain size can otherwise be handed in by a stale event
	// mid-resize.
	if (x < 0 || y < 0 || x >= width || y >= height)
		return 0;

	if (!ensure_depth_texture((Uint32)width, (Uint32)height))
		return 0;

	// Private RGBA8 target, swapchain-sized, released at the end of this
	// call -- picking is click-frequency, not per-frame, so there is no
	// need to keep it around the way g_depth_texture is.
	SDL_GPUTextureCreateInfo tex_info = {};
	tex_info.type = SDL_GPU_TEXTURETYPE_2D;
	tex_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	tex_info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
	tex_info.width = (Uint32)width;
	tex_info.height = (Uint32)height;
	tex_info.layer_count_or_depth = 1;
	tex_info.num_levels = 1;
	tex_info.sample_count = SDL_GPU_SAMPLECOUNT_1;

	SDL_GPUTexture *pick_texture =
	    SDL_CreateGPUTexture(g_device, &tex_info);
	if (pick_texture == nullptr) {
		SDL_Log("gpu: pick texture creation failed: %s",
		    SDL_GetError());
		return 0;
	}

	g_cmd = SDL_AcquireGPUCommandBuffer(g_device);
	if (g_cmd == nullptr) {
		SDL_Log("gpu: pick command buffer failed: %s", SDL_GetError());
		SDL_ReleaseGPUTexture(g_device, pick_texture);
		return 0;
	}

	// Reuses gpu_screenshot_*()'s "redirect the scene pass at an
	// offscreen target" trick (g_capture_texture) instead of a second
	// mechanism: gpu_scene_begin() already knows how to point
	// g_color_target/g_target_index at whatever g_capture_texture holds.
	// FSV_RENDER_SELECT makes node_set_color() (src/geometry.c) paint
	// flat id colors instead of lit real ones, and makes gpu_scene_end()
	// clear to (0,0,0,0) instead of the visible frame's dark-slate
	// background -- see the comment there.
	g_capture_texture = pick_texture;
	g_render_mode = FSV_RENDER_SELECT;

	gpu_scene_begin();
	geometry_draw(FALSE); // no text, no cursor -- exactly ogl_select_modern()
	gpu_scene_end();

	g_render_mode = FSV_RENDER_NORMAL;
	g_capture_texture = nullptr;

	// The window rect, clamped to the viewport. Texels the clamp cuts
	// off stay 0 in `ids` (the memset above).
	const int wx0 = MAX(0, x - R), wy0 = MAX(0, y - R);
	const int wx1 = MIN(width - 1, x + R), wy1 = MIN(height - 1, y + R);
	const int ww = wx1 - wx0 + 1, wh = wy1 - wy0 + 1;

	SDL_GPUTransferBufferCreateInfo transfer_info = {};
	transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
	transfer_info.size = (Uint32)(4 * ww * wh); // RGBA8 texels
	SDL_GPUTransferBuffer *download =
	    SDL_CreateGPUTransferBuffer(g_device, &transfer_info);
	if (download == nullptr) {
		SDL_Log("gpu: pick transfer buffer failed: %s", SDL_GetError());
		SDL_SubmitGPUCommandBuffer(g_cmd);
		g_cmd = nullptr;
		SDL_ReleaseGPUTexture(g_device, pick_texture);
		return 0;
	}

	unsigned int node_id = 0;
	{
		SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(g_cmd);
		SDL_GPUTextureRegion source = {};
		source.texture = pick_texture;
		source.x = (Uint32)wx0;
		// SDL_GPU texture regions are top-left origin (SDL_gpu.h:
		// SDL_GPUTextureRegion::y is "the *top* offset of the
		// region"), same as the swapchain and same as the (x, y)
		// input.cpp already hands in. ogl_select_modern()'s `yy =
		// viewport[3] - y` exists purely to convert into
		// glReadPixels()'s bottom-left-origin convention; there is
		// nothing to convert here. Verified by clicking a node near
		// the top vs. bottom of the window and confirming both
		// resolve to the node actually drawn there (an inverted flip
		// would have swapped top and bottom hits) -- see
		// docs/PORTING.md Task 4.2.
		source.y = (Uint32)wy0;
		source.w = (Uint32)ww;
		source.h = (Uint32)wh;
		source.d = 1;
		SDL_GPUTextureTransferInfo destination = {};
		destination.transfer_buffer = download;
		destination.offset = 0;
		destination.pixels_per_row = (Uint32)ww;
		destination.rows_per_layer = (Uint32)wh;
		SDL_DownloadFromGPUTexture(copy_pass, &source, &destination);
		SDL_EndGPUCopyPass(copy_pass);

		SDL_GPUFence *fence =
		    SDL_SubmitGPUCommandBufferAndAcquireFence(g_cmd);
		g_cmd = nullptr;
		if (fence == nullptr) {
			// Without a fence there is no way to know the copy
			// has landed -- mapping the transfer buffer now could
			// read stale or undefined bytes and hand back a
			// plausible-looking but bogus id. Fail the pick
			// instead of guessing, like every other failure path
			// in this function.
			SDL_Log("gpu: pick fence acquire failed: %s",
			    SDL_GetError());
			SDL_ReleaseGPUTransferBuffer(g_device, download);
			SDL_ReleaseGPUTexture(g_device, pick_texture);
			return 0;
		}
		SDL_WaitForGPUFences(g_device, true, &fence, 1);
		SDL_ReleaseGPUFence(g_device, fence);

		const Uint8 *pixels = (const Uint8 *)
		    SDL_MapGPUTransferBuffer(g_device, download, false);
		if (pixels == nullptr)
			SDL_Log("gpu: pick map failed: %s", SDL_GetError());
		else {
			// Byte order matches node_set_color()'s encode
			// (src/geometry.c: r = id & 0xFF, g = (id>>8) & 0xFF,
			// b = (id>>16) & 0xFF) and ogl_select_modern()'s
			// decode (src/ogl.c:456: color[0] + (color[1]<<8) +
			// (color[2]<<16)) exactly. Each downloaded texel
			// lands at its window-relative slot in `ids`; the
			// clamped-off border stays 0 from the memset.
			for (int gy = 0; gy < wh; gy++)
				for (int gx = 0; gx < ww; gx++) {
					const Uint8 *px =
					    pixels + 4 * (gy * ww + gx);
					const int ix = (wx0 + gx) - (x - R);
					const int iy = (wy0 + gy) - (y - R);
					ids[iy * GPU_PICK_WINDOW + ix] =
					    (unsigned int)px[0] |
					    ((unsigned int)px[1] << 8) |
					    ((unsigned int)px[2] << 16);
				}
			node_id = ids[R * GPU_PICK_WINDOW + R];
			SDL_UnmapGPUTransferBuffer(g_device, download);
		}
	}

	SDL_ReleaseGPUTransferBuffer(g_device, download);
	SDL_ReleaseGPUTexture(g_device, pick_texture);
	return node_id;
}

unsigned int
gpu_pick(int x, int y)
{
	unsigned int ids[GPU_PICK_WINDOW * GPU_PICK_WINDOW];

	return pick_impl(x, y, ids);
}

unsigned int
gpu_pick_window(int x, int y, unsigned int *ids)
{
	return pick_impl(x, y, ids);
}

// ---- --screenshot ----------------------------------------------------
//
// Renders one scene into an offscreen R8G8B8A8 texture and writes it out
// as a BMP, for CI smoke tests: "did the port actually draw anything?" is
// exactly the regression a headless check can catch. The caller drives
// the scene itself, so this is a pair of brackets rather than one call:
//
//   gpu_screenshot_begin(w, h);
//   gpu_scene_begin(); geometry_draw(TRUE); gpu_scene_end();
//   gpu_screenshot_end(path);
//
// gpu_scene_begin()/gpu_scene_end() need no special case beyond noticing
// g_capture_texture and switching color target + pipeline format.

bool
gpu_screenshot_begin(int width, int height)
{
	if (!g_ready || width <= 0 || height <= 0)
		return false;

	SDL_GPUTextureCreateInfo info = {};
	info.type = SDL_GPU_TEXTURETYPE_2D;
	info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
	info.width = (Uint32)width;
	info.height = (Uint32)height;
	info.layer_count_or_depth = 1;
	info.num_levels = 1;
	info.sample_count = SDL_GPU_SAMPLECOUNT_1;

	g_capture_texture = SDL_CreateGPUTexture(g_device, &info);
	if (g_capture_texture == nullptr) {
		SDL_Log("gpu: screenshot texture creation failed: %s",
		    SDL_GetError());
		return false;
	}
	g_capture_width = (Uint32)width;
	g_capture_height = (Uint32)height;

	if (!ensure_depth_texture(g_capture_width, g_capture_height)) {
		SDL_ReleaseGPUTexture(g_device, g_capture_texture);
		g_capture_texture = nullptr;
		return false;
	}

	g_cmd = SDL_AcquireGPUCommandBuffer(g_device);
	if (g_cmd == nullptr) {
		SDL_Log("gpu: screenshot command buffer failed: %s",
		    SDL_GetError());
		SDL_ReleaseGPUTexture(g_device, g_capture_texture);
		g_capture_texture = nullptr;
		return false;
	}
	return true;
}

bool
gpu_screenshot_end(const char *path)
{
	bool ok = false;

	if (g_capture_texture == nullptr || g_cmd == nullptr)
		return false;

	const Uint32 bytes = g_capture_width * g_capture_height * 4;

	SDL_GPUTransferBufferCreateInfo transfer_info = {};
	transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
	transfer_info.size = bytes;
	SDL_GPUTransferBuffer *download =
	    SDL_CreateGPUTransferBuffer(g_device, &transfer_info);
	if (download == nullptr) {
		SDL_Log("gpu: screenshot transfer buffer failed: %s",
		    SDL_GetError());
		SDL_SubmitGPUCommandBuffer(g_cmd);
		g_cmd = nullptr;
		goto out;
	}

	{
		SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(g_cmd);
		SDL_GPUTextureRegion source = {};
		source.texture = g_capture_texture;
		source.w = g_capture_width;
		source.h = g_capture_height;
		source.d = 1;
		SDL_GPUTextureTransferInfo destination = {};
		destination.transfer_buffer = download;
		destination.offset = 0;
		destination.pixels_per_row = g_capture_width;
		destination.rows_per_layer = g_capture_height;
		SDL_DownloadFromGPUTexture(copy_pass, &source, &destination);
		SDL_EndGPUCopyPass(copy_pass);

		SDL_GPUFence *fence =
		    SDL_SubmitGPUCommandBufferAndAcquireFence(g_cmd);
		g_cmd = nullptr;
		if (fence != nullptr) {
			SDL_WaitForGPUFences(g_device, true, &fence, 1);
			SDL_ReleaseGPUFence(g_device, fence);
		}

		void *pixels = SDL_MapGPUTransferBuffer(g_device, download, false);
		if (pixels == nullptr)
			SDL_Log("gpu: screenshot map failed: %s", SDL_GetError());
		else {
			// R8G8B8A8_UNORM is R,G,B,A in memory order, which is
			// what SDL_PIXELFORMAT_RGBA32 means on either endianness.
			SDL_Surface *surface = SDL_CreateSurfaceFrom(
			    (int)g_capture_width, (int)g_capture_height,
			    SDL_PIXELFORMAT_RGBA32, pixels,
			    (int)g_capture_width * 4);
			if (surface == nullptr)
				SDL_Log("gpu: SDL_CreateSurfaceFrom failed: %s",
				    SDL_GetError());
			else {
				ok = SDL_SaveBMP(surface, path);
				if (!ok)
					SDL_Log("gpu: SDL_SaveBMP failed: %s",
					    SDL_GetError());
				SDL_DestroySurface(surface);
			}
			SDL_UnmapGPUTransferBuffer(g_device, download);
		}
		SDL_ReleaseGPUTransferBuffer(g_device, download);
	}

out:
	SDL_ReleaseGPUTexture(g_device, g_capture_texture);
	g_capture_texture = nullptr;
	g_capture_width = g_capture_height = 0;
	return ok;
}

// ---- --record ----------------------------------------------------------
//
// Task 6.4's demo-video capture: like gpu_screenshot_*() above, but the
// offscreen texture is created in the swapchain's *own* pixel format
// (SDL_GetGPUSwapchainTextureFormat()) rather than a fixed
// R8G8B8A8_UNORM, and gpu_scene_begin() is told (via g_recording_frame)
// to draw into it with target index 0 -- the same scene/text pipelines a
// visible frame uses. That format match is what lets the caller also
// render ImGui's draw data into this texture using the *one* pipeline
// ImGui_ImplSDLGPU3_Init() ever builds (against that same swapchain
// format, in main.cpp) -- no second ImGui pipeline, no restructuring of
// the ImGui backend. The caller drives the whole frame:
//
//   SDL_GPUCommandBuffer *cmd = gpu_record_begin(w, h);
//   gpu_scene_begin(); geometry_draw(TRUE); gpu_scene_end();
//   // ImGui pass on `cmd`, target = gpu_record_texture(), LOADOP_LOAD
//   gpu_record_end("frame.bmp");
//
// gpu_record_begin() returns nullptr (having logged the reason) on
// failure, exactly like gpu_frame_begin() -- the caller must not call
// gpu_record_end() in that case. gpu_record_end() always releases the
// texture and clears g_recording_frame, whether or not the write
// succeeded.
SDL_GPUCommandBuffer *
gpu_record_begin(int width, int height)
{
	if (!g_ready || width <= 0 || height <= 0)
		return nullptr;

	SDL_GPUTextureCreateInfo info = {};
	info.type = SDL_GPU_TEXTURETYPE_2D;
	info.format = SDL_GetGPUSwapchainTextureFormat(g_device, g_window);
	info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
	info.width = (Uint32)width;
	info.height = (Uint32)height;
	info.layer_count_or_depth = 1;
	info.num_levels = 1;
	info.sample_count = SDL_GPU_SAMPLECOUNT_1;

	g_capture_texture = SDL_CreateGPUTexture(g_device, &info);
	if (g_capture_texture == nullptr) {
		SDL_Log("gpu: record texture creation failed: %s",
		    SDL_GetError());
		return nullptr;
	}
	g_capture_width = (Uint32)width;
	g_capture_height = (Uint32)height;
	g_recording_frame = true;

	if (!ensure_depth_texture(g_capture_width, g_capture_height)) {
		SDL_ReleaseGPUTexture(g_device, g_capture_texture);
		g_capture_texture = nullptr;
		g_recording_frame = false;
		return nullptr;
	}

	g_cmd = SDL_AcquireGPUCommandBuffer(g_device);
	if (g_cmd == nullptr) {
		SDL_Log("gpu: record command buffer failed: %s",
		    SDL_GetError());
		SDL_ReleaseGPUTexture(g_device, g_capture_texture);
		g_capture_texture = nullptr;
		g_recording_frame = false;
		return nullptr;
	}
	return g_cmd;
}

SDL_GPUTexture *
gpu_record_texture(void)
{
	return g_capture_texture;
}

bool
gpu_record_end(const char *path)
{
	bool ok = false;

	if (g_capture_texture == nullptr || g_cmd == nullptr) {
		g_recording_frame = false;
		return false;
	}

	// The texture is whatever SDL_GetGPUSwapchainTextureFormat()
	// returned in gpu_record_begin() -- confirmed B8G8R8A8_UNORM on
	// this port's actual target (macOS/Metal, see docs/PORTING.md),
	// but queried live rather than assumed. Map the two byte orders
	// SDL_GPU's swapchain formats can plausibly be to the matching
	// SDL_PixelFormat rather than hard-coding one, so a wrong guess
	// fails loudly (mis-mapping colors, not just a bad enum tag) on a
	// platform where the swapchain format ever differs.
	const SDL_GPUTextureFormat fmt =
	    SDL_GetGPUSwapchainTextureFormat(g_device, g_window);
	SDL_PixelFormat pixel_format;
	switch (fmt) {
	case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM:
		pixel_format = SDL_PIXELFORMAT_RGBA32;
		break;
	case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM:
		pixel_format = SDL_PIXELFORMAT_BGRA32;
		break;
	default:
		SDL_Log("gpu: record: unhandled swapchain format %d, "
		    "cannot decode captured pixels", (int)fmt);
		SDL_SubmitGPUCommandBuffer(g_cmd);
		g_cmd = nullptr;
		goto out;
	}

	{
		const Uint32 bytes = g_capture_width * g_capture_height * 4;

		SDL_GPUTransferBufferCreateInfo transfer_info = {};
		transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		transfer_info.size = bytes;
		SDL_GPUTransferBuffer *download =
		    SDL_CreateGPUTransferBuffer(g_device, &transfer_info);
		if (download == nullptr) {
			SDL_Log("gpu: record transfer buffer failed: %s",
			    SDL_GetError());
			SDL_SubmitGPUCommandBuffer(g_cmd);
			g_cmd = nullptr;
			goto out;
		}

		SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(g_cmd);
		SDL_GPUTextureRegion source = {};
		source.texture = g_capture_texture;
		source.w = g_capture_width;
		source.h = g_capture_height;
		source.d = 1;
		SDL_GPUTextureTransferInfo destination = {};
		destination.transfer_buffer = download;
		destination.offset = 0;
		destination.pixels_per_row = g_capture_width;
		destination.rows_per_layer = g_capture_height;
		SDL_DownloadFromGPUTexture(copy_pass, &source, &destination);
		SDL_EndGPUCopyPass(copy_pass);

		SDL_GPUFence *fence =
		    SDL_SubmitGPUCommandBufferAndAcquireFence(g_cmd);
		g_cmd = nullptr;
		if (fence != nullptr) {
			SDL_WaitForGPUFences(g_device, true, &fence, 1);
			SDL_ReleaseGPUFence(g_device, fence);
		}

		void *pixels = SDL_MapGPUTransferBuffer(g_device, download, false);
		if (pixels == nullptr)
			SDL_Log("gpu: record map failed: %s", SDL_GetError());
		else {
			SDL_Surface *surface = SDL_CreateSurfaceFrom(
			    (int)g_capture_width, (int)g_capture_height,
			    pixel_format, pixels, (int)g_capture_width * 4);
			if (surface == nullptr)
				SDL_Log("gpu: SDL_CreateSurfaceFrom failed: %s",
				    SDL_GetError());
			else {
				ok = SDL_SaveBMP(surface, path);
				if (!ok)
					SDL_Log("gpu: SDL_SaveBMP failed: %s",
					    SDL_GetError());
				SDL_DestroySurface(surface);
			}
			SDL_UnmapGPUTransferBuffer(g_device, download);
		}
		SDL_ReleaseGPUTransferBuffer(g_device, download);
	}

out:
	SDL_ReleaseGPUTexture(g_device, g_capture_texture);
	g_capture_texture = nullptr;
	g_capture_width = g_capture_height = 0;
	g_recording_frame = false;
	return ok;
}
