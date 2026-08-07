// src/sdl/main.cpp — SPDX-License-Identifier: MIT
//
// SDL3 + SDL_GPU (Metal on macOS) application shell for the fsv port.
// Owns the window, the main loop, the ImGui backends, the animation tick
// and the startup sequence (scan a directory, lay out its geometry, place
// the camera). The GPU device, pipelines and render passes belong to
// src/sdl/gpu.cpp; this file only sequences the frame:
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
#include <cstring>
extern "C" {
#include "common.h"
#include "animation.h"
#include "camera.h"
#include "color.h"
#include "fsv-platform.h"
#include "geometry.h"
#include "scanfs.h"
}

// ---- Platform hook implementations -----------------------------------
//
// libfsvcore (animation.c) calls through fsv_platform for anything
// frontend-specific. All five fields must be non-NULL once installed
// below.

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

// Scroll state for MapV/TreeV camera panning. Plain statics until the
// ImGui scrollbars exist (Task 5.1); camera.c only ever reads back what
// it wrote here, so panning behaves as if the user never scrolled.
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

// ---- Startup ---------------------------------------------------------
//
// The GTK frontend's fsv.c owns this sequence (fsv_load() then
// fsv_set_mode()), but fsv.c is a GtkApplication entry point and cannot
// be linked here. The two functions below are the GTK-free half of those
// two, in the same order and with the same arguments; the pieces left out
// are the ones that only exist to drive GTK widgets (window_set_access(),
// gui_update(), camera_update_scrollbars(), filelist_init()) and the
// two-second splash-screen sleep, which this frontend has no splash for.

// Port of fsv.c's initial_camera_pan("new_fs"): the opening fly-in, run
// one frame after the mode is set so that the first (slow) frame does not
// turn into a camera jump.
static void
initial_camera_pan(void *mesg)
{
	(void)mesg;
	// Keeps root_dnode from appearing twice in a row at the bottom of
	// the node history stack.
	G_LIST_PREPEND(globals.history, NULL);
	camera_look_at_full(root_dnode, MORPH_SIGMOID, 4.0);
}

// Port of fsv.c's fsv_set_mode(), FSV_NONE case ("filesystem's first
// appearance"). The switch-between-modes cases belong to Task 5.1's menu.
static void
enter_mode(FsvMode mode)
{
	geometry_init(mode);
	camera_init(mode, /* initial_view */ TRUE);
	globals.fsv_mode = mode;
	// schedule_event() is declared with an unprototyped parameter list
	// (void (*)()), which C++ will not implicitly convert to; animation.c
	// calls it back with one void * argument (its SchedEvent struct types
	// the field as void (*)(void *)).
	schedule_event((void (*)())initial_camera_pan, (char *)"new_fs", 1);
}

// Port of fsv.c's fsv_load(). Returns false if the scan produced nothing
// to draw.
static bool
load_filesystem(const char *dir, FsvMode mode)
{
	scanfs(dir);
	if (globals.fstree == NULL || root_dnode == NULL) {
		SDL_Log("fsv: nothing to visualize in \"%s\"", dir);
		return false;
	}

	g_list_free(globals.history);
	globals.history = NULL;
	globals.current_node = root_dnode;

	globals.fsv_mode = FSV_NONE;
	enter_mode(mode);
	return true;
}

// ---- Frame -----------------------------------------------------------

static void
draw_scene(void)
{
	gpu_scene_begin();
	// TRUE = high detail: node labels (Task 3.4) and the node cursor.
	// The GTK frontend passes TRUE from its render() callback too; only
	// picking passes FALSE.
	if (globals.fsv_mode != FSV_NONE)
		geometry_draw(TRUE);
	gpu_scene_end();
}

// ---- Command line ----------------------------------------------------

static void
usage(const char *argv0)
{
	SDL_Log("Usage: %s [rootdir] [--discv|--mapv|--treev] "
	    "[--screenshot FILE]", argv0);
}

int
main(int argc, char **argv)
{
	const char *root_dir = ".";
	const char *screenshot_path = nullptr;
	// Same default as the GTK frontend (src/fsv.c).
	FsvMode initial_mode = FSV_MAPV;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--discv") == 0)
			initial_mode = FSV_DISCV;
		else if (strcmp(argv[i], "--mapv") == 0)
			initial_mode = FSV_MAPV;
		else if (strcmp(argv[i], "--treev") == 0)
			initial_mode = FSV_TREEV;
		else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
			screenshot_path = argv[++i];
		else if (argv[i][0] == '-') {
			usage(argv[0]);
			return 1;
		} else
			root_dir = argv[i];
	}

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
	// call that might touch fsv_platform (scanfs() and the animation
	// tick both do).
	fsv_platform = {
		sdl_request_frame,
		sdl_render_frame,
		sdl_viewport_size,
		sdl_set_scroll,
		sdl_get_scroll,
	};

	// Sane camera state before anything is scanned: setup_projection_
	// matrix() runs on frames drawn before camera_init() (src/fsv.c does
	// the same thing for the same reason).
	camera->fov = 45.0;
	camera->near_clip = 1.0;
	camera->far_clip = 2.0;

	// Reads ~/.fsvrc, picks the color mode and builds the spectrum
	// table. Without it every node's color stays zeroed and the whole
	// scene renders black -- the GTK frontend calls this from
	// window_init() (src/window.c), which this frontend has no
	// equivalent of.
	color_init();

	if (!load_filesystem(root_dir, initial_mode))
		return 1;

	// --screenshot: no window loop, no ImGui. Let the intro camera pan
	// play out (morphs run on wall-clock time, so this has to actually
	// wait), then render one scene into an offscreen texture and write
	// it out. Bounded so a stuck animation can never hang CI.
	if (screenshot_path != nullptr) {
		const Uint64 deadline = SDL_GetTicks() + 8000;
		while (fsv_animation_tick() != 0 && SDL_GetTicks() < deadline)
			SDL_Delay(8);

		int w = 0, h = 0;
		SDL_GetWindowSizeInPixels(g_window, &w, &h);
		bool ok = gpu_screenshot_begin(w, h);
		if (ok) {
			draw_scene();
			ok = gpu_screenshot_end(screenshot_path);
		}
		if (ok)
			SDL_Log("fsv: wrote %s (%dx%d)", screenshot_path, w, h);

		SDL_WaitForGPUIdle(device);
		gpu_shutdown();
		SDL_DestroyWindow(g_window);
		SDL_Quit();
		return ok ? 0 : 1;
	}

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
		// No ImGui windows yet: the menu bar and the dirtree/filelist
		// panels are Tasks 5.1/5.2. The backends stay wired up (and
		// the pass below stays in the frame) so that landing them is
		// a matter of adding widgets here, not re-plumbing the frame.
		ImGui::Render();

		ImDrawData *draw_data = ImGui::GetDrawData();
		const bool empty_draw = draw_data->CmdListsCount == 0 ||
		    draw_data->DisplaySize.x <= 0.0f ||
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

			// Pass 1: the 3-D scene. Clears color + depth.
			draw_scene();

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
