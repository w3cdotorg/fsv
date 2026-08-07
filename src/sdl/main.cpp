// src/sdl/main.cpp — SPDX-License-Identifier: MIT
//
// SDL3 + SDL_GPU (Metal on macOS) application shell for the fsv port.
// Owns the window, the main loop, the ImGui backends and the animation
// tick. The GPU device, pipelines and render passes belong to
// src/sdl/gpu.cpp (Task 3.2); this file only sequences the frame:
//
//   gpu_frame_begin() -> ImGui PrepareDrawData -> scene pass ->
//   ImGui pass -> gpu_frame_end()
//
// Identifiers below are cross-checked against the vendored ImGui example
// (subprojects/imgui/example_sdl3_sdlgpu3_main.cpp.txt, v1.92.9b-docking)
// rather than trusted from memory.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>
#include "gpu.h"
#include "gpu_internal.hpp"
extern "C" {
#include "fsv-platform.h"
}

// ---- Platform hook implementations -----------------------------------
//
// libfsvcore (animation.c) calls through fsv_platform for anything
// frontend-specific. All five fields must be non-NULL once installed
// below; render_frame is a no-op until Milestone 3 wires actual scene
// drawing in here.

static bool g_frame_requested = true; // render at least the first frame
static SDL_Window *g_window = nullptr;

static void
sdl_request_frame(void)
{
	g_frame_requested = true;
}

static void
sdl_render_frame(void)
{
	// Called from inside fsv_animation_tick(), i.e. before this
	// iteration's command buffer exists -- so, like the GTK frontend's
	// ogl_draw() (gtk_gl_area_queue_render()), this only *schedules* a
	// redraw. The loop below does the drawing. It cannot spin: the core
	// only calls this while globals.need_redraw is set, which the tick
	// clears once the animation reaches steady state.
	g_frame_requested = true;
}

static void
sdl_viewport_size(int *width, int *height)
{
	SDL_GetWindowSizeInPixels(g_window, width, height);
}

// Scroll state for MapV/TreeV camera panning. Plain statics for now —
// camera.c doesn't drive any scrolling until geometry.c (M3) exists, so
// there's nothing yet to exercise this beyond satisfying the non-NULL
// hook contract.
static double g_scroll[2];

static void
sdl_set_scroll(int axis, double lower, double upper, double page, double pos)
{
	(void)lower;
	(void)upper;
	(void)page;
	g_scroll[axis] = pos;
}

static double
sdl_get_scroll(int axis)
{
	return g_scroll[axis];
}

int
main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	if (!SDL_Init(SDL_INIT_VIDEO)) {
		SDL_Log("SDL_Init failed: %s", SDL_GetError());
		return 1;
	}

	g_window = SDL_CreateWindow("fsv", 1280, 800,
	    SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	if (g_window == nullptr) {
		SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
		return 1;
	}

	// Creates the GPU device, claims the window, builds the scene
	// pipelines. Everything below needs all of that, so bail on a
	// partial init rather than running on and re-logging the same
	// failure every frame; gpu_init() has already logged the reason.
	gpu_init(g_window);
	if (!gpu_ready())
		return 1;
	SDL_GPUDevice *device = gpu_device();

	// Install all five hooks the core requires before any libfsvcore
	// call that might touch fsv_platform (fsv_animation_tick(), below).
	fsv_platform = {
		sdl_request_frame,
		sdl_render_frame,
		sdl_viewport_size,
		sdl_set_scroll,
		sdl_get_scroll,
	};

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::StyleColorsDark();

	ImGui_ImplSDL3_InitForSDLGPU(g_window);
	ImGui_ImplSDLGPU3_InitInfo init_info = {};
	init_info.Device = device;
	init_info.ColorTargetFormat =
	    SDL_GetGPUSwapchainTextureFormat(device, g_window);
	init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
	ImGui_ImplSDLGPU3_Init(&init_info);

	bool running = true;
	while (running) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			ImGui_ImplSDL3_ProcessEvent(&ev);
			if (ev.type == SDL_EVENT_QUIT)
				running = false;
			// Any event (input, resize, expose, ...) may warrant a
			// redraw. ImGui_ImplSDL3_ProcessEvent() just queues input
			// internally when called outside of a NewFrame/Render
			// pair, so it's safe to keep draining the event queue
			// below without rendering yet -- the queued input is
			// consumed at the next ImGui::NewFrame() once we do
			// render. This is also how window resize/expose events
			// end up triggering a re-render: they're regular events.
			g_frame_requested = true;
		}
		if (!running)
			break;

		// Mirror the vendored example: don't bother building or
		// rendering a frame while minimized.
		if (SDL_GetWindowFlags(g_window) & SDL_WINDOW_MINIMIZED) {
			SDL_Delay(10);
			continue;
		}

		bool animating = fsv_animation_tick() != 0;

		if (!animating && !g_frame_requested) {
			// Nothing to draw this iteration: block until the next
			// event (or 16ms, whichever comes first) instead of
			// unconditionally building+submitting a frame every
			// ~16ms, so idle CPU actually stays near zero.
			SDL_WaitEventTimeout(nullptr, 16);
			continue;
		}
		g_frame_requested = false;

		ImGui_ImplSDLGPU3_NewFrame();
		ImGui_ImplSDL3_NewFrame();
		ImGui::NewFrame();
		ImGui::ShowDemoWindow();
		ImGui::Render();

		ImDrawData *draw_data = ImGui::GetDrawData();
		const bool empty_draw = draw_data->DisplaySize.x <= 0.0f ||
		    draw_data->DisplaySize.y <= 0.0f;

		SDL_GPUCommandBuffer *cmd = gpu_frame_begin();
		if (cmd == nullptr)
			continue;
		SDL_GPUTexture *swapchain = gpu_frame_swapchain_texture();

		if (swapchain != nullptr) {
			// Mandatory before any render pass: uploads ImGui's
			// vertex/index buffers for this frame (a copy pass,
			// which cannot be nested inside a render pass).
			if (!empty_draw)
				ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, cmd);

			// Pass 1: the 3-D scene. Clears color + depth. No
			// geometry to draw until Task 3.3 ports geometry.c,
			// so for now this is exactly the clear that Task 2.2
			// did -- but through the real pipeline and depth
			// attachment.
			gpu_scene_begin();
			gpu_scene_end();

			// Pass 2: ImGui on top of the same swapchain texture,
			// LOADOP_LOAD so the scene survives, and with no depth
			// target so the UI is never depth-tested.
			if (!empty_draw) {
				SDL_GPUColorTargetInfo target = {};
				target.texture = swapchain;
				target.load_op = SDL_GPU_LOADOP_LOAD;
				target.store_op = SDL_GPU_STOREOP_STORE;

				SDL_GPURenderPass *pass =
				    SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
				ImGui_ImplSDLGPU3_RenderDrawData(draw_data, cmd, pass);
				SDL_EndGPURenderPass(pass);
			}
		}

		gpu_frame_end();
	}

	// Shutdown order per the vendored example: platform backend, then
	// renderer backend, then the ImGui context, then SDL objects.
	SDL_WaitForGPUIdle(device);
	ImGui_ImplSDL3_Shutdown();
	ImGui_ImplSDLGPU3_Shutdown();
	ImGui::DestroyContext();

	gpu_shutdown();
	SDL_DestroyWindow(g_window);
	SDL_Quit();

	return 0;
}
