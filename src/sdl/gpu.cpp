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
#include "tmaptext.h" /* text_upload_mvp( ) -- see gpu_upload_matrices() */
}

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
enum { NUM_PRIMS = 2, NUM_DEPTH_TESTS = 3, NUM_TARGETS = 2 };
SDL_GPUGraphicsPipeline *g_pipelines[NUM_PRIMS][NUM_DEPTH_TESTS][NUM_TARGETS];

SDL_GPUTexture *g_depth_texture;
SDL_GPUTextureFormat g_depth_format;
Uint32 g_depth_width, g_depth_height;

// Per-frame state, valid between gpu_frame_begin() and gpu_frame_end().
SDL_GPUCommandBuffer *g_cmd;
SDL_GPUTexture *g_swapchain;
Uint32 g_swapchain_width, g_swapchain_height;

// --screenshot: when non-null, the scene renders into this offscreen
// R8G8B8A8 texture instead of the swapchain (see gpu_screenshot_begin()).
SDL_GPUTexture *g_capture_texture;
Uint32 g_capture_width, g_capture_height;

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
	depth_stencil_state.enable_stencil_test = false;

	// No blending: the GL frontend sets a blend func but never enables
	// GL_BLEND for scene geometry (only the text overlay uses it, which
	// is Task 3.4's pipeline).
	SDL_GPUColorTargetDescription color_target_desc = {};
	color_target_desc.format = target == 0
	    ? SDL_GetGPUSwapchainTextureFormat(g_device, g_window)
	    : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	color_target_desc.blend_state.enable_blend = false;

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
	g_color_target = g_capture_texture != nullptr ? g_capture_texture
						      : g_swapchain;
	g_target_index = g_capture_texture != nullptr ? 1 : 0;
	if (g_cmd == nullptr || g_color_target == nullptr || !g_ready)
		return;

	// Same three calls the GTK frontend's render() made per frame
	// (src/ogl.c:432).
	setup_projection_matrix();
	setup_modelview_matrix();
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
	color_target.clear_color = SDL_FColor{ 0.08f, 0.10f, 0.12f, 1.0f };
	color_target.load_op = SDL_GPU_LOADOP_CLEAR;
	color_target.store_op = SDL_GPU_STOREOP_STORE;

	SDL_GPUDepthStencilTargetInfo depth_target = {};
	depth_target.texture = g_depth_texture;
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

unsigned int
gpu_pick(int x, int y)
{
	/* Task 4.2: set g_render_mode to FSV_RENDER_SELECT, run
	 * gpu_scene_begin()/geometry_draw()/gpu_scene_end() against an
	 * offscreen R8G8B8A8 texture (pipeline_for()'s target == 1 already
	 * builds for that format), then SDL_DownloadFromGPUTexture() one
	 * pixel and decode the id. geometry.c paints its own id colors --
	 * see node_set_color(), src/geometry.c. */
	(void)x;
	(void)y;
	return 0;
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
