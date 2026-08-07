// src/sdl/gpu.cpp — SPDX-License-Identifier: MIT
//
// SDL_GPU renderer core for the fsv macOS/Metal port: device + pipeline
// creation, the FsvMesh vertex/index buffer API, the camera matrices
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

#include "gpu_internal.hpp"

extern "C" {
#include "common.h"
#include "camera.h"
}

// Byte arrays for shaders/compiled/scene.{vert,frag}.{msl,spv}, generated
// at build time by tools/embed-shaders.py.
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
// contract too (40-byte stride, attributes at 0/12/24).
static_assert(sizeof(FsvVertex) == 40, "scene pipeline vertex stride");
static_assert(offsetof(FsvVertex, normal) == 12, "vertex attribute 1 offset");
static_assert(offsetof(FsvVertex, color) == 24, "vertex attribute 2 offset");

// ---- Module state ----------------------------------------------------

SDL_Window *g_window;
SDL_GPUDevice *g_device;

SDL_GPUGraphicsPipeline *g_scene_pipeline;
// Same shaders and state as g_scene_pipeline, but targeting an offscreen
// R8G8B8A8_UNORM texture instead of the swapchain: a pipeline's color
// target format is fixed at creation, and Task 4.2 renders id-colors into
// a readback texture of a format it controls (the swapchain's is the
// driver's choice, and may be BGRA or sRGB). Picking needs no separate
// *shader*: fsv_mesh_draw_id() just pushes a flat color with lighting
// off, exactly as geometry.c does today in RENDERMODE_SELECT.
SDL_GPUGraphicsPipeline *g_id_pipeline;

SDL_GPUTexture *g_depth_texture;
SDL_GPUTextureFormat g_depth_format;
Uint32 g_depth_width, g_depth_height;

// Per-frame state, valid between gpu_frame_begin() and gpu_frame_end().
SDL_GPUCommandBuffer *g_cmd;
SDL_GPUTexture *g_swapchain;
Uint32 g_swapchain_width, g_swapchain_height;
SDL_GPURenderPass *g_scene_pass;

// Shadow copies of the two uniform blocks. Every draw call re-pushes both
// (as the shader headers say it will): SDL_GPU uniform pushes go into a
// per-command-buffer ring buffer, so this is cheaper than tracking
// dirtiness, and it matches the GL frontend's habit of setting the color
// uniform before nearly every glDrawElements.
SceneVertUBO g_vert_ubo;
SceneFragUBO g_frag_ubo;

// The base modelview matrix: right-handed, +z straight up, camera at the
// origin looking down -x. Private, unlike gl.base_modelview in ogl.h --
// nothing outside this file ever read it.
mat4 g_base_modelview;

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
    int num_candidates, Uint32 num_uniform_buffers)
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
		info.num_samplers = 0;
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

// The color/depth-independent half of both pipelines.
SDL_GPUGraphicsPipeline *
create_scene_pipeline(SDL_GPUShader *vert, SDL_GPUShader *frag,
    SDL_GPUTextureFormat color_format)
{
	SDL_GPUVertexBufferDescription vertex_buffer_desc = {};
	vertex_buffer_desc.slot = 0;
	vertex_buffer_desc.pitch = sizeof(FsvVertex);
	vertex_buffer_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
	vertex_buffer_desc.instance_step_rate = 0;

	SDL_GPUVertexAttribute vertex_attributes[3] = {};
	vertex_attributes[0].location = 0; // scene.vert: in vec3 position
	vertex_attributes[0].buffer_slot = 0;
	vertex_attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
	vertex_attributes[0].offset = offsetof(FsvVertex, pos);
	vertex_attributes[1].location = 1; // scene.vert: in vec3 normal
	vertex_attributes[1].buffer_slot = 0;
	vertex_attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
	vertex_attributes[1].offset = offsetof(FsvVertex, normal);
	vertex_attributes[2].location = 2; // reserved: per-vertex color
	vertex_attributes[2].buffer_slot = 0;
	vertex_attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
	vertex_attributes[2].offset = offsetof(FsvVertex, color);

	SDL_GPUVertexInputState vertex_input_state = {};
	vertex_input_state.vertex_buffer_descriptions = &vertex_buffer_desc;
	vertex_input_state.num_vertex_buffers = 1;
	vertex_input_state.vertex_attributes = vertex_attributes;
	vertex_input_state.num_vertex_attributes = 3;

	// ogl_init(): glEnable(GL_CULL_FACE) with GL's defaults, i.e. cull
	// back faces, front faces wound counter-clockwise.
	SDL_GPURasterizerState rasterizer_state = {};
	rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
	rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
	rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

	// ogl_init(): glEnable(GL_DEPTH_TEST) with GL's default GL_LESS.
	SDL_GPUDepthStencilState depth_stencil_state = {};
	depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
	depth_stencil_state.enable_depth_test = true;
	depth_stencil_state.enable_depth_write = true;
	depth_stencil_state.enable_stencil_test = false;

	// No blending: the GL frontend sets a blend func but never enables
	// GL_BLEND for scene geometry (only the text overlay uses it, which
	// is Task 3.4's pipeline).
	SDL_GPUColorTargetDescription color_target_desc = {};
	color_target_desc.format = color_format;
	color_target_desc.blend_state.enable_blend = false;

	SDL_GPUGraphicsPipelineTargetInfo target_info = {};
	target_info.color_target_descriptions = &color_target_desc;
	target_info.num_color_targets = 1;
	target_info.depth_stencil_format = g_depth_format;
	target_info.has_depth_stencil_target = true;

	SDL_GPUGraphicsPipelineCreateInfo info = {};
	info.vertex_shader = vert;
	info.fragment_shader = frag;
	info.vertex_input_state = vertex_input_state;
	info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	info.rasterizer_state = rasterizer_state;
	info.depth_stencil_state = depth_stencil_state;
	info.target_info = target_info;

	return SDL_CreateGPUGraphicsPipeline(g_device, &info);
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

	mat4 frustum;
	glm_frustum_rh_zo(-dx, dx, -dy, dy, camera->near_clip,
	    camera->far_clip, frustum);
	glm_mat4_identity(gpu_mat.projection);
	glm_mat4_mul(gpu_mat.projection, frustum, gpu_mat.projection);
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

} // namespace

// ---- Public API ------------------------------------------------------

FsvGpuMatrices gpu_mat;

// Port of ogl_upload_matrices() (src/ogl.c:318). The GL original ended in
// glUniformMatrix4fv(); here the values land in the shadow UBO and are
// pushed to the command buffer by the next draw call, which means this
// stays callable outside a render pass (geometry.c calls it while walking
// the tree, and picking calls it with no frame in flight at all).
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

	SDL_GPUShader *vert = create_shader(SDL_GPU_SHADERSTAGE_VERTEX,
	    vert_blobs, (int)SDL_arraysize(vert_blobs), /* uniform buffers */ 1);
	SDL_GPUShader *frag = create_shader(SDL_GPU_SHADERSTAGE_FRAGMENT,
	    frag_blobs, (int)SDL_arraysize(frag_blobs), /* uniform buffers */ 1);
	if (vert == nullptr || frag == nullptr) {
		SDL_Log("gpu: no usable scene shader");
		return;
	}

	g_scene_pipeline = create_scene_pipeline(vert, frag,
	    SDL_GetGPUSwapchainTextureFormat(g_device, g_window));
	if (g_scene_pipeline == nullptr)
		SDL_Log("gpu: scene pipeline creation failed: %s",
		    SDL_GetError());

	g_id_pipeline = create_scene_pipeline(vert, frag,
	    SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
	if (g_id_pipeline == nullptr)
		SDL_Log("gpu: id pipeline creation failed: %s", SDL_GetError());

	// The pipelines hold their own references to the shader modules.
	SDL_ReleaseGPUShader(g_device, vert);
	SDL_ReleaseGPUShader(g_device, frag);

	if (g_scene_pipeline != nullptr && g_id_pipeline != nullptr)
		SDL_Log("gpu: scene + id pipelines created");

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
}

void
gpu_shutdown(void)
{
	if (g_device == nullptr)
		return;

	if (g_scene_pipeline != nullptr)
		SDL_ReleaseGPUGraphicsPipeline(g_device, g_scene_pipeline);
	if (g_id_pipeline != nullptr)
		SDL_ReleaseGPUGraphicsPipeline(g_device, g_id_pipeline);
	if (g_depth_texture != nullptr)
		SDL_ReleaseGPUTexture(g_device, g_depth_texture);
	g_scene_pipeline = nullptr;
	g_id_pipeline = nullptr;
	g_depth_texture = nullptr;

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
	if (g_cmd == nullptr || g_swapchain == nullptr ||
	    g_scene_pipeline == nullptr)
		return;

	// Same three calls the GTK frontend's render() made per frame
	// (src/ogl.c:432).
	setup_projection_matrix();
	setup_modelview_matrix();
	gpu_upload_matrices();

	SDL_GPUColorTargetInfo color_target = {};
	color_target.texture = g_swapchain;
	// Unchanged from Task 2.2's clear color, so this task's visual
	// result stays comparable. src/ogl.c cleared to transparent black;
	// switching is Task 3.3's call, once there is geometry to see.
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

	g_scene_pass = SDL_BeginGPURenderPass(g_cmd, &color_target, 1,
	    &depth_target);
	if (g_scene_pass == nullptr) {
		SDL_Log("gpu: SDL_BeginGPURenderPass failed: %s",
		    SDL_GetError());
		return;
	}
	SDL_BindGPUGraphicsPipeline(g_scene_pass, g_scene_pipeline);
}

void
gpu_scene_end(void)
{
	if (g_scene_pass == nullptr)
		return;
	SDL_EndGPURenderPass(g_scene_pass);
	g_scene_pass = nullptr;
}

unsigned int
gpu_pick(int x, int y)
{
	/* Task 4.2: render the scene through g_id_pipeline into an
	 * offscreen R8G8B8A8 texture with fsv_mesh_draw_id(), then
	 * SDL_DownloadFromGPUTexture() one pixel and decode the id. */
	(void)x;
	(void)y;
	return 0;
}

// ---- Meshes ----------------------------------------------------------

struct FsvMesh {
	SDL_GPUBuffer *vertex_buffer;
	SDL_GPUBuffer *index_buffer;
	Uint32 vertex_capacity; // bytes
	Uint32 index_capacity;  // bytes
	Uint32 num_indices;
};

namespace {

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

} // namespace

FsvMesh *
fsv_mesh_new(void)
{
	return (FsvMesh *)SDL_calloc(1, sizeof(FsvMesh));
}

void
fsv_mesh_free(FsvMesh *m)
{
	if (m == nullptr)
		return;
	if (m->vertex_buffer != nullptr)
		SDL_ReleaseGPUBuffer(g_device, m->vertex_buffer);
	if (m->index_buffer != nullptr)
		SDL_ReleaseGPUBuffer(g_device, m->index_buffer);
	SDL_free(m);
}

void
fsv_mesh_upload(FsvMesh *m, const FsvVertex *verts, int nverts,
    const unsigned int *indices, int nindices)
{
	if (m == nullptr || g_device == nullptr)
		return;
	if (verts == nullptr || indices == nullptr || nverts <= 0 ||
	    nindices <= 0) {
		m->num_indices = 0;
		return;
	}

	const Uint32 vertex_bytes = (Uint32)nverts * sizeof(FsvVertex);
	const Uint32 index_bytes = (Uint32)nindices * sizeof(unsigned int);

	if (!ensure_buffer(&m->vertex_buffer, &m->vertex_capacity, vertex_bytes,
	        SDL_GPU_BUFFERUSAGE_VERTEX) ||
	    !ensure_buffer(&m->index_buffer, &m->index_capacity, index_bytes,
	        SDL_GPU_BUFFERUSAGE_INDEX)) {
		m->num_indices = 0;
		return;
	}

	// One transfer buffer holds both halves back to back.
	SDL_GPUTransferBufferCreateInfo transfer_info = {};
	transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transfer_info.size = vertex_bytes + index_bytes;

	SDL_GPUTransferBuffer *transfer =
	    SDL_CreateGPUTransferBuffer(g_device, &transfer_info);
	if (transfer == nullptr) {
		SDL_Log("gpu: SDL_CreateGPUTransferBuffer failed: %s",
		    SDL_GetError());
		m->num_indices = 0;
		return;
	}

	void *mapped = SDL_MapGPUTransferBuffer(g_device, transfer, false);
	if (mapped == nullptr) {
		SDL_Log("gpu: SDL_MapGPUTransferBuffer failed: %s",
		    SDL_GetError());
		SDL_ReleaseGPUTransferBuffer(g_device, transfer);
		m->num_indices = 0;
		return;
	}
	memcpy(mapped, verts, vertex_bytes);
	memcpy((char *)mapped + vertex_bytes, indices, index_bytes);
	SDL_UnmapGPUTransferBuffer(g_device, transfer);

	// Uploads run on their own command buffer: meshes are rebuilt on
	// rescan, not per frame, so this never has to join the frame's.
	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g_device);
	if (cmd == nullptr) {
		SDL_Log("gpu: upload command buffer failed: %s", SDL_GetError());
		SDL_ReleaseGPUTransferBuffer(g_device, transfer);
		m->num_indices = 0;
		return;
	}
	SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(cmd);

	SDL_GPUTransferBufferLocation source = {};
	SDL_GPUBufferRegion destination = {};

	source.transfer_buffer = transfer;
	source.offset = 0;
	destination.buffer = m->vertex_buffer;
	destination.offset = 0;
	destination.size = vertex_bytes;
	SDL_UploadToGPUBuffer(copy_pass, &source, &destination, false);

	source.offset = vertex_bytes;
	destination.buffer = m->index_buffer;
	destination.offset = 0;
	destination.size = index_bytes;
	SDL_UploadToGPUBuffer(copy_pass, &source, &destination, false);

	SDL_EndGPUCopyPass(copy_pass);
	SDL_SubmitGPUCommandBuffer(cmd);
	SDL_ReleaseGPUTransferBuffer(g_device, transfer);

	m->num_indices = (Uint32)nindices;
}

void
fsv_mesh_draw(FsvMesh *m)
{
	SDL_assert(g_scene_pass != nullptr); // must be inside the scene pass
	if (g_scene_pass == nullptr || m == nullptr || m->num_indices == 0)
		return;

	SDL_PushGPUVertexUniformData(g_cmd, 0, &g_vert_ubo, sizeof g_vert_ubo);
	SDL_PushGPUFragmentUniformData(g_cmd, 0, &g_frag_ubo, sizeof g_frag_ubo);

	SDL_GPUBufferBinding vertex_binding = {};
	vertex_binding.buffer = m->vertex_buffer;
	SDL_BindGPUVertexBuffers(g_scene_pass, 0, &vertex_binding, 1);

	SDL_GPUBufferBinding index_binding = {};
	index_binding.buffer = m->index_buffer;
	SDL_BindGPUIndexBuffer(g_scene_pass, &index_binding,
	    SDL_GPU_INDEXELEMENTSIZE_32BIT);

	SDL_DrawGPUIndexedPrimitives(g_scene_pass, m->num_indices, 1, 0, 0, 0);
}

void
fsv_mesh_draw_id(FsvMesh *m, unsigned int id)
{
	// Same encoding ogl_select_modern() decodes (src/ogl.c:498):
	// red = id[7:0], green = id[15:8], blue = id[23:16].
	const SceneFragUBO saved_frag = g_frag_ubo;
	const std::int32_t saved_vert_lighting = g_vert_ubo.lightning_enabled;

	gpu_set_lighting(0);
	gpu_set_color((float)(id & 0xffu) / 255.0f,
	    (float)((id >> 8) & 0xffu) / 255.0f,
	    (float)((id >> 16) & 0xffu) / 255.0f, 1.0f);

	fsv_mesh_draw(m);

	g_frag_ubo = saved_frag;
	g_vert_ubo.lightning_enabled = saved_vert_lighting;
}
