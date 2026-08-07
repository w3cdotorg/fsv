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
#include "app.h"
#include "gpu.h"
#include "gpu_internal.hpp"
#include "input.h"
#include "ui_main.h"
#include <cstring>
extern "C" {
#include "common.h"
#include "animation.h"
#include "camera.h"
#include "color.h"
#include "fsv-platform.h"
#include "geometry.h"
#include "scanfs.h"
#include "tmaptext.h" /* text_init( ) */
#include "window.h" /* StatusBarID, window_statusbar( ) */
}

// ---- Platform hook implementations -----------------------------------
//
// libfsvcore (animation.c) calls through fsv_platform for anything
// frontend-specific. All five fields must be non-NULL once installed
// below.

static bool g_frame_requested = true; // render at least the first frame
static SDL_Window *g_window = nullptr;

static bool g_imgui_ready = false;   // ImGui backends initialized
static bool g_quit_requested = false;
// Set only around scanfs(). gui_update() is the core's generic "let the
// frontend breathe" call and fires from colexp.c and common.c too, which
// Tasks 4.1/5.2 will make reachable -- from inside the main loop, i.e.
// potentially between its imgui_new_frame() and submit_frame(). Rendering
// a nested frame there would re-enter ImGui::NewFrame(). During the scan
// there is no such outer frame, and the main loop is not running, which
// is exactly when this frontend needs to drive one itself.
static bool g_scanning = false;

// Root directory last (successfully) handed to scanfs() -- app_root_dir(),
// Rescan's implicit target, the window title, and the Change Root dialog's
// default location. Owned here; xstrdup()'d/xfree()'d, never a borrowed
// pointer (see load_filesystem() below).
static char *g_root_dir = nullptr;

// A Change Root/Rescan request queued by ui_main.cpp's menu bar (or the
// async folder-dialog callback), applied by app_apply_pending_root_
// change() once the main loop has closed out the frame that queued it --
// see app.h's doc comment for why this can't run synchronously from
// inside ui_main_draw(). g_pending_new_root == NULL means "Rescan" (reuse
// g_root_dir); non-NULL means "Change Root" to that directory.
static bool g_pending_root_change = false;
static char *g_pending_new_root = nullptr;

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

// Full port of fsv.c's initial_camera_pan(): run one frame after the mode
// is set (schedule_event(..., 1) below) so that the first (slow) frame
// does not turn into a camera jump. mesg == "new_fs" is the filesystem's
// first appearance (load_filesystem()'s slow 4-second fly-in to root);
// anything else is a same-filesystem mode switch (Task 5.1's Vis menu,
// via app_switch_mode() below), which pans to wherever the camera already
// was instead of jumping back to root.
static void
initial_camera_pan(void *mesg)
{
	// Keeps root_dnode from appearing twice in a row at the bottom of
	// the node history stack.
	G_LIST_PREPEND(globals.history, NULL);

	const char *m = (const char *)mesg;
	if (m != NULL && strcmp(m, "new_fs") == 0) {
		camera_look_at_full(root_dnode, MORPH_SIGMOID, 4.0);
	} else if (globals.fsv_mode == FSV_TREEV) {
		// Enter TreeV mode with an L-shaped pan.
		camera_treev_lpan_look_at(globals.current_node, 1.0);
	} else {
		camera_look_at_full(globals.current_node, MORPH_INV_QUADRATIC, 1.0);
	}
}

// Port of fsv.c's fsv_set_mode(), FSV_NONE case ("filesystem's first
// appearance"). app_switch_mode() below is the other case (switching
// modes on an already-loaded filesystem).
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

// app.h: Vis menu -> mode switch. Port of fsv.c's fsv_set_mode() for
// every case *except* FSV_NONE (enter_mode() above is that one) --
// callbacks.c's on_vis_*_activate() is the GTK analogue of this half:
// same geometry_init()/camera_init()/schedule_event() sequence, with
// camera_init()'s initial_view argument FALSE (this is not the
// filesystem's first appearance) and the "" pan message instead of
// "new_fs" (a short pan from wherever the camera already is, not the
// slow root fly-in).
void
app_switch_mode(int mode_int)
{
	const FsvMode mode = (FsvMode)mode_int;

	if (g_scanning || globals.fsv_mode == FSV_NONE)
		return; // nothing loaded yet, or a scan owns the main thread
	if (globals.fsv_mode == mode)
		return; // matches callbacks.c's on_vis_*_activate guard

	geometry_init(mode);
	camera_init(mode, /* initial_view */ FALSE);
	globals.fsv_mode = mode;
	// fsv_set_mode()'s `about(ABOUT_END)` ("ensure that About presentation
	// is not up") is deliberately not ported here: this frontend has no
	// About/splash 3D presentation to dismiss in the first place (Task
	// 3.3's about.c deviation) -- stubs.c's about() is an unconditional
	// FALSE no-op, so the call would be permanently inert.
	schedule_event((void (*)())initial_camera_pan, (char *)"", 1);
}

// Port of fsv.c's fsv_load(). Returns false if the scan produced nothing
// to draw.
static bool
load_filesystem(const char *dir, FsvMode mode)
{
	// Before scanning, not after: the scan renders progress frames (see
	// gui_update()), and FSV_NONE is what tells draw_scene() there is no
	// geometry to walk yet. globals.fsv_mode is zero-initialized, which
	// is FSV_DISCV, not FSV_NONE.
	globals.fsv_mode = FSV_NONE;

	g_scanning = true;
	scanfs(dir);
	g_scanning = false;

	if (globals.fstree == NULL || root_dnode == NULL) {
		SDL_Log("fsv: nothing to visualize in \"%s\"", dir);
		return false;
	}

	g_list_free(globals.history);
	globals.history = NULL;
	globals.current_node = root_dnode;

	// Track the root actually scanned (app_root_dir(), Rescan, the
	// window title, the Change Root dialog's default location) and
	// reflect it in the title -- the brief's addition over the GTK
	// frontend, which titles its window a bare "fsv" always.
	//
	// Deliberately xgetcwd(), not dir: scanfs() just chdir()'d into dir
	// and never chdir's back (src/scanfs.c:320), so the process's
	// working directory *is* the new root from here on. Storing the
	// resolved absolute path rather than the caller's (possibly
	// relative) dir string is what makes Rescan safe to call twice --
	// storing dir verbatim would have Rescan hand a now-stale relative
	// path (e.g. "src") back to scanfs(), which chdir()s *again*,
	// relative to the directory the first scan already left us in
	// (".../src/src") and fatally g_error()s when that doesn't exist.
	// xstrdup() before freeing the old value: g_root_dir itself has
	// already been chdir()'d away from by the time we get here, so
	// there is no aliasing hazard either way, but the order still costs
	// nothing.
	char *new_root_dir = xstrdup(xgetcwd());
	if (g_root_dir != nullptr)
		xfree(g_root_dir);
	g_root_dir = new_root_dir;
	if (g_window != nullptr) {
		char title[1024];
		SDL_snprintf(title, sizeof(title), "fsv - %s", g_root_dir);
		SDL_SetWindowTitle(g_window, title);
	}

	enter_mode(mode);
	return true;
}

// app.h: File -> Rescan / Change Root..., and the async folder-dialog
// callback below. Never calls scanfs() itself -- see app_apply_pending_
// root_change()'s doc comment in app.h for why that has to wait until
// after the current ImGui frame is closed.
void
app_request_rescan(void)
{
	if (g_scanning)
		return;
	g_pending_root_change = true;
	if (g_pending_new_root != nullptr)
		xfree(g_pending_new_root);
	g_pending_new_root = nullptr; // NULL = reuse g_root_dir
}

// File-local: only the folder-dialog callback below needs this (ui_main.cpp
// reaches Change Root through app_show_change_root_dialog() instead, since
// it never has a path in hand -- SDL_ShowOpenFolderDialog() is
// asynchronous).
static void
app_request_change_root(const char *dir)
{
	if (g_scanning)
		return;
	g_pending_root_change = true;
	char *copy = xstrdup(dir);
	if (g_pending_new_root != nullptr)
		xfree(g_pending_new_root);
	g_pending_new_root = copy;
}

// app.h: called from main()'s loop right after submit_frame(), i.e. once
// the frame that queued a request (if any) is fully closed out.
void
app_apply_pending_root_change(void)
{
	if (!g_pending_root_change)
		return;
	g_pending_root_change = false;

	const char *dir = g_pending_new_root != nullptr ?
	    g_pending_new_root : g_root_dir;
	// globals.fsv_mode is never FSV_NONE here (a scan can't be running
	// while this is reached -- g_scanning gates both request functions
	// above), so this just carries the current mode across the rescan.
	// The FSV_MAPV fallback is unreachable in practice; kept because
	// load_filesystem() requires *some* mode and "unreachable" is not
	// "impossible to make reachable by a future change here".
	const FsvMode mode = globals.fsv_mode != FSV_NONE ?
	    globals.fsv_mode : FSV_MAPV;

	load_filesystem(dir, mode); // logs and no-ops on failure, as at startup

	if (g_pending_new_root != nullptr)
		xfree(g_pending_new_root);
	g_pending_new_root = nullptr;
}

bool
app_is_scanning(void)
{
	return g_scanning;
}

const char *
app_root_dir(void)
{
	return g_root_dir != nullptr ? g_root_dir : "";
}

// app.h: File -> Change Root.... Opens SDL3's native folder picker
// (dialog_change_root()'s GTK counterpart is gtk_file_chooser_dialog_new
// with GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, i.e. gui_dir_choose()).
//
// SDL_ShowOpenFolderDialog()'s callback "may be invoked from a different
// thread than the one the function was invoked on" (SDL_dialog.h) --
// true in practice on at least one platform SDL supports (its Linux/XDG
// portal path runs over DBus), so this cannot assume it is safe to touch
// any of this file's statics (g_pending_root_change, g_pending_new_root)
// directly from inside the callback. SDL_RunOnMainThread() is SDL3's own
// answer to exactly this: "If this is called on the main thread, the
// callback is executed immediately. If this is called on another
// thread, this callback is queued for execution on the main thread
// during event processing" (SDL_init.h) -- safe unconditionally,
// regardless of which thread actually invokes the dialog callback on
// this platform.
static void
apply_folder_choice_on_main_thread(void *userdata)
{
	char *path = static_cast<char *>(userdata);
	app_request_change_root(path);
	SDL_free(path);
}

static void
folder_dialog_callback(void *userdata, const char *const *filelist, int filter)
{
	(void)userdata;
	(void)filter;

	if (filelist == nullptr) {
		SDL_Log("fsv: Change Root dialog error: %s", SDL_GetError());
		return;
	}
	if (filelist[0] == nullptr)
		return; // user canceled, or chose nothing

	// "The filelist argument should not be freed; it will automatically
	// be freed when the callback returns" (SDL_dialog.h) -- so the
	// string has to be copied now, before handing it to
	// SDL_RunOnMainThread(), whose queued case (a different thread)
	// only runs the copy above *after* this function has already
	// returned and SDL has freed filelist.
	char *path = SDL_strdup(filelist[0]);
	SDL_RunOnMainThread(apply_folder_choice_on_main_thread, path, false);
}

void
app_show_change_root_dialog(void)
{
	if (g_scanning)
		return;
	SDL_ShowOpenFolderDialog(folder_dialog_callback, nullptr, g_window,
	    g_root_dir, false);
}

// ---- Frame -----------------------------------------------------------

static void
draw_scene(void)
{
	gpu_scene_begin();
	// TRUE = high detail: node labels (Task 3.4) and the node cursor.
	// The GTK frontend passes TRUE from its render() callback too; only
	// picking passes FALSE.
	//
	// FSV_NONE means "no geometry laid out yet" -- during the scan, and
	// between load_filesystem() and enter_mode(). Note FSV_NONE is 4, not
	// 0, so this guard only holds because load_filesystem() assigns it
	// before scanning; a zero-initialized globals.fsv_mode reads as
	// FSV_DISCV and would send geometry_draw() into an empty tree.
	if (globals.fsv_mode != FSV_NONE)
		geometry_draw(TRUE);
	gpu_scene_end();
}

// Builds this frame's ImGui content between these two: imgui_new_frame(),
// widgets, ImGui::Render(), submit_frame(). Split this way because the
// scan-progress frames below need the same command-buffer/pass sequence
// with different (and much simpler) UI content.
static void
imgui_new_frame(void)
{
	ImGui_ImplSDLGPU3_NewFrame();
	ImGui_ImplSDL3_NewFrame();
	ImGui::NewFrame();
}

// Records and submits one frame: the scene pass, then ImGui on top of the
// same swapchain texture. Expects ImGui::Render() to have been called.
static void
submit_frame(void)
{
	ImDrawData *draw_data = ImGui::GetDrawData();
	const bool empty_draw = draw_data == nullptr ||
	    draw_data->CmdListsCount == 0 ||
	    draw_data->DisplaySize.x <= 0.0f ||
	    draw_data->DisplaySize.y <= 0.0f;

	SDL_GPUCommandBuffer *cmd = gpu_frame_begin();
	if (cmd == nullptr) {
		// Nothing was drawn, so the redraw this frame owed is still
		// owed. Without re-arming, the loop can go back to blocking in
		// SDL_WaitEventTimeout() and sit on a stale image until some
		// unrelated event happens to wake it.
		g_frame_requested = true;
		return;
	}

	SDL_GPUTexture *swapchain = gpu_frame_swapchain_texture();
	if (swapchain == nullptr) {
		// Same reasoning; the command buffer still has to be submitted
		// (gpu_frame_end()) to release the acquired swapchain.
		g_frame_requested = true;
	} else {
		// Mandatory before any render pass: uploads ImGui's
		// vertex/index buffers for this frame (a copy pass, which
		// cannot be nested inside a render pass).
		if (!empty_draw)
			ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, cmd);

		// Pass 1: the 3-D scene. Clears color + depth.
		draw_scene();

		// Pass 2: ImGui on top of the same swapchain texture,
		// LOADOP_LOAD so the scene survives, and with no depth target
		// so the UI is never depth-tested.
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

// ---- Scan progress ---------------------------------------------------
//
// scanfs() blocks this thread for the whole scan -- minutes on a large
// tree. The GTK frontend stays alive through it because scanfs.c calls
// gui_update() after every directory entry (scanfs.c:174) and gui.c's
// implementation iterates the GLib main loop, which repaints and services
// the window manager. With a no-op gui_update() the SDL window never
// pumps its event queue and macOS marks the app "Not Responding".
//
// So gui_update() is implemented here rather than stubbed: it pumps
// events, keeps ImGui fed, and paints a progress overlay. It is called
// per directory entry, hence the time-based throttle -- rendering a frame
// per entry would dominate the scan.

// Latest message per status bar, as pushed by the core. scanfs.c puts
// "Scanning: <dir>" in SB_RIGHT for every directory it enters, which is
// the progress readout this frontend has; SB_LEFT gets a stats/sec figure
// from scan_monitor(), a GLib timeout that never fires here (there is no
// GLib main loop), so it stays empty in practice.
static char g_statusbar[2][512];

extern "C" void
window_statusbar(StatusBarID sb_id, const char *message)
{
	const int i = (sb_id == SB_LEFT) ? 0 : 1;
	SDL_strlcpy(g_statusbar[i], message != NULL ? message : "",
	    sizeof(g_statusbar[i]));
}

extern "C" void
gui_update(void)
{
	if (!g_scanning || !g_imgui_ready)
		return; // outside the scan, or --screenshot: nothing to drive

	// ~100ms between frames. The scan calls this per directory entry
	// (tens of thousands of times per second on a warm cache), so the
	// throttle is what keeps the progress display from costing more
	// than the scan itself.
	static Uint64 last_ms = 0;
	const Uint64 now = SDL_GetTicks();
	if (last_ms != 0 && now - last_ms < 100)
		return;
	last_ms = now;

	SDL_Event ev;
	while (SDL_PollEvent(&ev)) {
		ImGui_ImplSDL3_ProcessEvent(&ev);
		if (ev.type == SDL_EVENT_QUIT)
			// scanfs() has no abort path, so this cannot stop the
			// scan; main() checks the flag once it returns and
			// exits before opening the main loop. The window stays
			// responsive in the meantime, which is the point.
			g_quit_requested = true;
	}

	if (SDL_GetWindowFlags(g_window) & SDL_WINDOW_MINIMIZED)
		return;

	imgui_new_frame();

	// A plain overlay in the corner: no title bar, no interaction, no
	// saved settings. Nothing else is on screen during the scan.
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(
	    ImVec2(vp->WorkPos.x + 20.0f, vp->WorkPos.y + 20.0f));
	ImGui::SetNextWindowBgAlpha(0.0f);
	if (ImGui::Begin("##scan", nullptr,
	    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
	    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
	    ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs)) {
		ImGui::Text("Scanning%s", g_quit_requested ?
		    " (quitting after this scan)..." : "...");
		if (g_statusbar[1][0] != '\0')
			ImGui::TextUnformatted(g_statusbar[1]);
		if (g_statusbar[0][0] != '\0')
			ImGui::TextUnformatted(g_statusbar[0]);
	}
	ImGui::End();
	ImGui::Render();

	// globals.fsv_mode is FSV_NONE throughout the scan, so the scene
	// pass inside submit_frame() clears the window and draws nothing.
	submit_frame();
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

	// Uploads the glyph atlas and builds the text pipeline (Task 3.4):
	// needs the device gpu_init() just created, so it cannot run any
	// earlier. Port of ogl_init()'s trailing text_init() call.
	text_init();

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

	// Before the scan, not after: scanning a large tree takes minutes,
	// and gui_update() paints its progress overlay through these
	// backends the whole time. --screenshot skips them (and so renders
	// no progress frames) because it never opens a window loop.
	if (screenshot_path == nullptr) {
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
		g_imgui_ready = true;
	}

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

	// A quit arriving during the scan cannot abort scanfs(), so it takes
	// effect here instead, before the main loop opens.
	bool running = !g_quit_requested;
	while (running) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			ImGui_ImplSDL3_ProcessEvent(&ev);
			if (ev.type == SDL_EVENT_QUIT)
				running = false;
			// ImGui gets the event first (above); input_handle_event()
			// checks io.WantCaptureMouse itself before navigating, so
			// a click/drag over an ImGui window never moves the
			// camera. Mouse nav (Task 4.1) and node selection
			// (gpu_pick(), Task 4.2) are both fully live now.
			input_handle_event(&ev);
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

		imgui_new_frame();
		// The dirtree/filelist panels are Task 5.2's; the menu bar,
		// node context menu and Help windows are this task's.
		ui_main_draw();
		ImGui::Render();
		submit_frame();

		// Deliberately *after* submit_frame(), not inside
		// ui_main_draw(): a queued Rescan/Change Root request runs
		// scanfs() here, which drives its own gui_update() progress-
		// overlay frames. Doing that from inside ui_main_draw() would
		// re-enter ImGui::NewFrame() while this iteration's own
		// NewFrame()/Render() pair (above) is still open. See app.h.
		app_apply_pending_root_change();
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
