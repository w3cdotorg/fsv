// src/sdl/main.cpp — SPDX-License-Identifier: MIT
//
// SDL3 + SDL_GPU (Metal on macOS) application skeleton for the fsv port.
// Owns the window, the GPU device, the main loop and the ImGui backends.
// No scene rendering yet (that's Milestone 3): this task only proves the
// window/device/ImGui/animation-tick wiring is correct end to end.
//
// gpu_init()/gpu_begin_frame()/gpu_end_frame() referenced in the task
// brief are intentionally NOT introduced here as a separate header: the
// real gpu.h/gpu.cpp module lands in Task 3.2. Until then the handful of
// GPU calls a real frontend needs live inline in main(), matching YAGNI.
//
// Identifiers below are cross-checked against the vendored ImGui example
// (subprojects/imgui/example_sdl3_sdlgpu3_main.cpp.txt, v1.92.9b-docking)
// rather than trusted from memory.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>
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
	// Scene render lands here in M3 (src/sdl/gpu.cpp). Nothing to draw
	// yet: main()'s loop below still clears + presents every frame.
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

	SDL_GPUDevice *device = SDL_CreateGPUDevice(
	    SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_SPIRV,
	    /* debug */ true, nullptr);
	if (device == nullptr) {
		SDL_Log("SDL_CreateGPUDevice failed: %s", SDL_GetError());
		return 1;
	}
	SDL_Log("GPU driver: %s", SDL_GetGPUDeviceDriver(device));

	g_window = SDL_CreateWindow("fsv", 1280, 800,
	    SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	if (g_window == nullptr) {
		SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
		return 1;
	}

	if (!SDL_ClaimWindowForGPUDevice(device, g_window)) {
		SDL_Log("SDL_ClaimWindowForGPUDevice failed: %s", SDL_GetError());
		return 1;
	}

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
		}

		bool animating = fsv_animation_tick() != 0;

		ImGui_ImplSDLGPU3_NewFrame();
		ImGui_ImplSDL3_NewFrame();
		ImGui::NewFrame();
		ImGui::ShowDemoWindow();
		ImGui::Render();

		ImDrawData *draw_data = ImGui::GetDrawData();
		const bool minimized = draw_data->DisplaySize.x <= 0.0f ||
		    draw_data->DisplaySize.y <= 0.0f;

		SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
		SDL_GPUTexture *swapchain = nullptr;
		SDL_WaitAndAcquireGPUSwapchainTexture(cmd, g_window, &swapchain,
		    nullptr, nullptr);

		if (swapchain != nullptr && !minimized) {
			// Mandatory before the render pass: uploads vertex/index
			// buffers for this frame's draw data.
			ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, cmd);

			SDL_GPUColorTargetInfo target = {};
			target.texture = swapchain;
			target.clear_color = SDL_FColor{ 0.08f, 0.10f, 0.12f, 1.0f };
			target.load_op = SDL_GPU_LOADOP_CLEAR;
			target.store_op = SDL_GPU_STOREOP_STORE;

			SDL_GPURenderPass *pass =
			    SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
			ImGui_ImplSDLGPU3_RenderDrawData(draw_data, cmd, pass);
			SDL_EndGPURenderPass(pass);
		}

		SDL_SubmitGPUCommandBuffer(cmd);

		// Idle-wait when nothing requested a frame and no animation is
		// in flight, so idle CPU stays near zero instead of spinning.
		if (!animating && !g_frame_requested)
			SDL_WaitEventTimeout(nullptr, 16);
		g_frame_requested = false;
	}

	// Shutdown order per the vendored example: platform backend, then
	// renderer backend, then the ImGui context, then SDL objects.
	SDL_WaitForGPUIdle(device);
	ImGui_ImplSDL3_Shutdown();
	ImGui_ImplSDLGPU3_Shutdown();
	ImGui::DestroyContext();

	SDL_ReleaseWindowFromGPUDevice(device, g_window);
	SDL_DestroyGPUDevice(device);
	SDL_DestroyWindow(g_window);
	SDL_Quit();

	return 0;
}
