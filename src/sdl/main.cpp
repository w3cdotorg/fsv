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
#include "ui_dialogs.h"
#include "ui_main.h"
#include "ui_overview.h"
#include "ui_panels.h"
#include "ui_rail.h"
#include <cstring>
#include <string>
extern "C" {
#include "common.h"
#include "animation.h"
#include "camera.h"
#include "colexp.h" /* --record: colexp( ), scripted directory expand */
#include "color.h"
#include "dirtree.h" /* --record: dirtree_entry_expanded( ) */
#include "fontatlas.h" /* font_atlas_find_font( ): the panels' TTF too */
#include "fsv-platform.h"
#include "geometry.h"
#include "scanfs.h"
#include "tmaptext.h" /* text_init( ) */
#include "window.h" /* StatusBarID, window_statusbar( ) */
}

#include "fsn-style.h" /* FSN_LANDSCAPE_CLASSIC -- see enter_fsn_mode_landscape() */

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

// Set for the duration of run_record_mode() (--record). The recording
// loop draws the real menu bar -- that is the point, the menus are meant
// to be visible in the demo video -- and pumps real SDL events, so its
// File menu items are genuinely clickable by whoever is at the keyboard.
// But that loop never calls app_apply_pending_root_change(), so a Rescan
// or Change Root queued there would sit in g_pending_root_change and be
// discarded at exit: a menu item that silently does nothing. Rather than
// teach the recording loop to run scanfs() mid-capture (which would tear
// down the tree the script's cues hold GNode * into), ui_main.cpp greys
// those two items out while recording -- see app_is_recording().
static bool g_recording = false;

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

// Scroll state for MapV/TreeV camera panning. Plain statics stand in for
// a real GtkAdjustment: camera.c only ever reads back what it wrote here
// (fsv_platform.get_scroll()), so panning behaves as if the user never
// scrolled -- until fsn-mode Task A3's camera rail (src/sdl/ui_rail.cpp)
// became the first real consumer. lower/upper/page used to be discarded
// ((void)-cast) since nothing read them back; ui_rail.cpp's Tilt/Height
// sliders need the full ScrollState to render a sensible range, so all
// four fields are kept now (see app_get_scroll_range() below).
struct ScrollAxisState {
	double lower = 0.0, upper = 100.0, page = 100.0, value = 0.0;
};
static ScrollAxisState g_scroll[2];

static void
sdl_set_scroll(int axis, double lower, double upper, double page, double pos)
{
	g_scroll[axis] = { lower, upper, page, pos };
}

static double
sdl_get_scroll(int axis)
{
	return g_scroll[axis].value;
}

// app.h: src/sdl/ui_rail.cpp's Tilt/Height sliders.
void
app_get_scroll_range(int axis, double *lower, double *upper, double *page,
    double *value)
{
	const ScrollAxisState &s = g_scroll[axis];
	*lower = s.lower;
	*upper = s.upper;
	*page = s.page;
	*value = s.value;
}

// app.h: src/sdl/ui_rail.cpp's Tilt/Height sliders, on user drag.
void
app_scrollbar_dragged(int axis, double new_value)
{
	g_scroll[axis].value = new_value;
	camera_scrollbar_moved(axis);
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

// fsn-mode Task B3: entering FSN mode auto-selects "classic"
// (fsn-style.h's FSN_LANDSCAPE_CLASSIC) unless the user has ever chosen a
// landscape explicitly from the Display menu (color.c's
// landscape_explicit(), set only by landscape_set(), the menu's own
// entry point). Runs on every FSN entry, not just the first -- a session
// that has never made an explicit choice should always land on classic
// in FSN, regardless of what an earlier non-FSN session left as the
// generic "landscape" nvstore value. landscape_set_auto() is the
// non-explicit twin of landscape_set() for exactly this: it applies and
// persists the preset without claiming to be the user's own choice, so a
// later explicit pick still overrides it and this default keeps
// reapplying until one is made. Leaving FSN restores nothing -- no
// "landscape before FSN" is saved anywhere -- so whatever FSN leaves
// selected simply stays selected afterward; keeping that asymmetry
// simple was a deliberate call, not an oversight (see docs/PORTING.md).
static void
enter_fsn_mode_landscape(FsvMode mode)
{
	if (mode == FSV_FSN && !landscape_explicit())
		landscape_set_auto(FSN_LANDSCAPE_CLASSIC);
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
	enter_fsn_mode_landscape(mode);
	// schedule_event() is declared with an unprototyped parameter list
	// (void (*)()), which C++ will not implicitly convert to; animation.c
	// calls it back with one void * argument (its SchedEvent struct types
	// the field as void (*)(void *)).
	schedule_event((void (*)())initial_camera_pan, (char *)"new_fs", 1);
}

// Shared body of app_switch_mode() and app_reset_camera() below: the
// "switch into this mode on an already-loaded filesystem" sequence
// (geometry_init()/camera_init()/schedule_event(), camera_init()'s
// initial_view argument FALSE, the "" short-pan message instead of
// "new_fs"'s slow root fly-in). Factored out because Reset needs to run
// this exact body for the *current* mode, which app_switch_mode()'s own
// mode == globals.fsv_mode guard exists specifically to reject.
static void
run_mode_entry(FsvMode mode)
{
	geometry_init(mode);
	camera_init(mode, /* initial_view */ FALSE);
	globals.fsv_mode = mode;
	enter_fsn_mode_landscape(mode);
	schedule_event((void (*)())initial_camera_pan, (char *)"", 1);
}

// app.h: Vis menu -> mode switch. Port of fsv.c's fsv_set_mode() for
// every case *except* FSV_NONE (enter_mode() above is that one) --
// callbacks.c's on_vis_*_activate() is the GTK analogue of this half.
void
app_switch_mode(int mode_int)
{
	const FsvMode mode = (FsvMode)mode_int;

	if (g_scanning || globals.fsv_mode == FSV_NONE)
		return; // nothing loaded yet, or a scan owns the main thread
	if (globals.fsv_mode == mode)
		return; // matches callbacks.c's on_vis_*_activate guard

	// fsv_set_mode()'s `about(ABOUT_END)` ("ensure that About presentation
	// is not up") is deliberately not ported here: this frontend has no
	// About/splash 3D presentation to dismiss in the first place (Task
	// 3.3's about.c deviation) -- stubs.c's about() is an unconditional
	// FALSE no-op, so the call would be permanently inert.
	run_mode_entry(mode);
}

// app.h: fsn-mode Task A3's camera rail -- "Reset" button. Same body as
// app_switch_mode(), minus its "already in this mode" guard (Reset's
// whole point is to re-enter the mode we're already in).
void
app_reset_camera(void)
{
	if (g_scanning || globals.fsv_mode == FSV_NONE)
		return;

	// If bird's-eye view is active, back it out through the camera's own
	// dormant API first rather than leaving birdseye_view_active TRUE
	// behind camera_init()'s fresh, non-birdseye pose (camera_init() does
	// not itself touch that flag -- verified by reading src/camera.c --
	// so a bare run_mode_entry() here would silently desync
	// birdseye_view_active from what the rail, and any subsequent
	// "Birds eye" click, believe the state to be). No new camera math:
	// camera_birdseye_view( ) already exists and already knows how to
	// restore a "before bird's-eye" pose.
	//
	// NOT the same as the user manually exiting bird's-eye view first,
	// though: a manual exit lets that restore morph run to completion
	// (several seconds) before anything else touches the camera. Here,
	// camera_init() (inside run_mode_entry(), below) fires on the very
	// next line and raw-overwrites camera->theta/phi/distance/near_clip/
	// far_clip and the mode's target fields *without* cancelling
	// whatever morph is still mid-flight on those exact variables --
	// camera_init() only ever assigns, it never calls morph_break(). The
	// pan_part master morph camera_birdseye_view( ) also just started
	// would otherwise survive too, and morph_iteration( ) (src/
	// animation.c) runs once per main-loop tick *before* this frame's
	// scheduled events -- including the schedule_event( ) below -- get a
	// chance to run, so the still-live backout morph would re-apply an
	// interpolated (birdseye-ish) value on top of camera_init( )'s fresh
	// pose for one frame, and the subsequently scheduled initial_camera_
	// pan( ) would then compute its own pan from that corrupted starting
	// point instead of the clean reset pose. camera_pan_break( ) is the
	// same call camera_look_at_full( )/camera_birdseye_view( ) themselves
	// already make before installing a *new* set of morphs on these same
	// variables -- confirmed by reading it: it switches on globals.
	// fsv_mode (still the pre-run_mode_entry() mode here, which is what
	// we want) and breaks exactly camera->theta/phi/distance/fov/
	// near_clip/far_clip/pan_part plus that mode's target fields, a
	// superset of what camera_birdseye_view(FALSE) just started morphing
	// -- so this fully neutralizes it. Deliberately not morph_break_all()
	// (src/animation.h): that call's own doc comment scopes it to "the
	// code that is about to destroy that [filesystem] tree" (scanfs()'s
	// teardown), and would also silently cancel any unrelated in-flight
	// colexp() expand/collapse animation -- a wider blast radius than
	// this Reset button has any business causing.
	if (window_birdseye_active()) {
		camera_birdseye_view(FALSE);
		window_birdseye_set_active(FALSE);
		camera_pan_break();
	}

	run_mode_entry(globals.fsv_mode);
}

// Port of fsv.c's fsv_load(). Returns false if the scan produced nothing
// to draw.
static bool
load_filesystem(const char *dir, FsvMode mode)
{
	// Everything that outlives a frame and points into the tree
	// scanfs() is about to free has to be dropped first. scanfs() does
	// the core's own share (morph_break_all() + scheduled_events_clear(),
	// which is shared with the GTK frontend); these two are this
	// frontend's:
	//
	//   - camera_pan_break() is strictly belt-and-suspenders, since
	//     morph_break_all() inside scanfs() would take the same camera
	//     morphs out a moment later. It runs anyway because it is the
	//     camera module's own documented "stop panning" entry point, so
	//     a future change to either side keeps the camera consistent
	//     without depending on the core's teardown order.
	//     morph_break() is silent on a variable that isn't being
	//     morphed, so the later blanket purge finds nothing left to
	//     free here and there is no double-free either way.
	//     Deliberately above the FSV_NONE assignment: camera_pan_break()
	//     switches on globals.fsv_mode and SWITCH_FAILs on FSV_NONE.
	//   - input_reset() drops input.cpp's indicated node, pending
	//     context-menu request and mouse grab -- see input.h.
	camera_pan_break();
	input_reset();

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

bool
app_is_recording(void)
{
	return g_recording;
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

		// Pass 0 (fsn-mode Task C1, FSN mode only, and only when
		// something it shows has moved): the overview mini-map, into
		// its own small texture with its own depth buffer. It has to
		// happen on this command buffer and before the ImGui pass
		// below, which samples that texture -- SDL_GPU orders passes
		// within a command buffer, so "earlier pass writes, later pass
		// reads" needs no explicit synchronization. Before the scene
		// pass rather than between it and ImGui only for readability;
		// the two are independent.
		ui_overview_render();

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

// ---- --record ----------------------------------------------------------
//
// Task 6.4: an offscreen, scripted "fly the camera around this checkout's
// own src/ tree" recording, for a short demo video. Reuses the exact
// frame shape submit_frame() above builds (ImGui::Render() while the
// window's own swapchain-format pipelines are still what's bound, then
// PrepareDrawData -> scene pass -> ImGui pass), but through
// gpu_record_begin()/gpu_record_texture()/gpu_record_end() instead of
// gpu_frame_*()/the swapchain -- see the comment in src/sdl/gpu.cpp's
// "--record" section for why that offscreen texture can carry ImGui's
// pass at all (format-matched to the swapchain, not a fixed capture
// format).
//
// Morphs (camera_look_at_full(), colexp()) time themselves off the real
// wall clock (animation.c's xgettime(), same source --screenshot's
// intro-pan wait already relies on -- see main()'s screenshot branch
// below), not off how many ticks have been called. So this loop paces
// itself to real time too, one iteration per ~1/30s of wall clock, and
// keys its own script off that same wall clock -- otherwise a loop that
// (being fully offscreen, no vsync) runs faster than real time would
// let the morphs race ahead of the frame numbers meant to capture them,
// and a slower one would leave them lagging. Pacing here is what makes
// "frame N happens at video-time N/30s" and "the morph is M seconds into
// a 2-second pan" agree.

// One entry in the scripted timeline below: at t_seconds (wall-clock
// seconds since recording started), call action(). Each fires once.
struct RecordCue {
	double t_seconds;
	void (*action)(void);
};

// The nodes the script flies to, resolved once up front (see
// run_record_mode()) from this checkout's own src/ tree -- gpu.cpp is
// this port's renderer core, and src/sdl/ is the directory this whole
// SDD phase has been building. A path missing from a future checkout
// (renamed file, different tree) degrades to "that cue's node is
// nullptr and camera_look_at_full()/colexp() are skipped", not a crash.
static GNode *g_record_sdl_dir;
static GNode *g_record_gpu_cpp;
static GNode *g_record_geometry_c;

static void
record_cue_expand_sdl(void)
{
	if (g_record_sdl_dir == nullptr)
		return;
	// Same guard ui_main.cpp's context menu uses before offering
	// "Expand": colexp() asserts NODE_IS_DIR(dnode), and this path is
	// already known to be a directory (record_find_node() below only
	// hands back whatever node_named() resolved, but the demo script
	// picks a path it knows is a directory).
	if (!dirtree_entry_expanded(g_record_sdl_dir))
		colexp(g_record_sdl_dir, COLEXP_EXPAND);
	camera_look_at_full(g_record_sdl_dir, MORPH_SIGMOID, 2.2);
}

static void
record_cue_look_gpu_cpp(void)
{
	if (g_record_gpu_cpp == nullptr)
		return;
	// Belt-and-suspenders, exactly like ui_dialogs.cpp's "Go to" button:
	// the previous cue already expanded src/sdl/, but camera_look_at_
	// full()'s DEBUG assert (parent directory must be expanded) is worth
	// satisfying unconditionally rather than relying on cue ordering.
	if (NODE_IS_DIR(g_record_gpu_cpp->parent) &&
	    !dirtree_entry_expanded(g_record_gpu_cpp->parent))
		colexp(g_record_gpu_cpp->parent, COLEXP_EXPAND_ANY);
	camera_look_at_full(g_record_gpu_cpp, MORPH_SIGMOID, 2.3);
}

// Points the switch straight at geometry.c instead of leaving it to land
// wherever the mode switch's own automatic pan would otherwise take it.
// app_switch_mode()'s enter path schedules initial_camera_pan() one tick
// later, which -- entering TreeV -- calls camera_treev_lpan_look_at(
// globals.current_node, 1.0); globals.current_node is still whatever the
// *previous* cue last looked at (src/sdl/gpu.cpp), so without this the
// camera would L-pan to gpu.cpp's new TreeV position first and only then
// need a *second*, separate reorientation to reach geometry.c -- two
// stacked camera cuts instead of one clean pan into the new mode.
static void
record_cue_switch_treev(void)
{
	if (g_record_geometry_c != nullptr)
		globals.current_node = g_record_geometry_c;
	app_switch_mode((int)FSV_TREEV);
}

// Continuous cues (dolly, revolve) are driven per-frame by t-ranges in
// run_record_mode() below, not one-shot RecordCue entries -- camera_
// dolly()/camera_revolve() are immediate deltas (the same calls input.cpp
// makes per mouse-motion event), not morphs, so "smooth" here means
// "called with a small delta every recorded frame across the range".
static const RecordCue g_record_cues[] = {
	{ 4.6, record_cue_expand_sdl },
	{ 7.2, record_cue_look_gpu_cpp },
	{ 11.6, record_cue_switch_treev },
};

// Resolves a script target by path relative to the recorded root (app_
// root_dir(), the directory scanfs() actually chdir()'d into -- see
// load_filesystem()) into the matching GNode, via common.c's node_named(
// ), which does the same absolute-path/component-walk resolution ui_
// dialogs.cpp's symlink-target lookup already relies on.
static GNode *
record_find_node(const char *relpath)
{
	std::string abspath = app_root_dir();
	abspath += "/";
	abspath += relpath;
	return node_named(abspath.c_str());
}

// Task 6.4's demo-video capture. Runs for `duration_seconds` (already
// loaded/mode-entered filesystem required -- main() only calls this after
// load_filesystem() succeeds, same precondition the normal loop has).
// Writes `frame_00000.bmp`, `frame_00001.bmp`, ... into `outdir` at a
// fixed 30fps; tools/make-demo.sh turns those into the actual mp4/gif.
// Returns false (having logged the reason) if the output directory
// couldn't be created or the very first frame couldn't be captured;
// a mid-recording capture failure just skips that one frame rather than
// aborting the whole recording, matching --screenshot's "fail loud, but
// only for what actually failed" style.
static bool
run_record_mode(const char *outdir, double duration_seconds)
{
	if (!SDL_CreateDirectory(outdir)) {
		SDL_Log("fsv: --record: could not create \"%s\": %s", outdir,
		    SDL_GetError());
		return false;
	}

	int width = 0, height = 0;
	SDL_GetWindowSizeInPixels(g_window, &width, &height);
	if (width <= 0 || height <= 0) {
		SDL_Log("fsv: --record: invalid window size");
		return false;
	}

	// Set here, past the two early returns above and immediately before
	// the first frame ui_main_draw() can run for: greys out File ->
	// Change Root.../Rescan for the whole recording. See g_recording's
	// declaration for why.
	g_recording = true;

	g_record_sdl_dir = record_find_node("sdl");
	g_record_gpu_cpp = record_find_node("sdl/gpu.cpp");
	g_record_geometry_c = record_find_node("geometry.c");
	if (g_record_sdl_dir == nullptr || g_record_gpu_cpp == nullptr ||
	    g_record_geometry_c == nullptr)
		SDL_Log("fsv: --record: one or more scripted targets not "
		    "found under \"%s\" -- recording will skip those cues",
		    app_root_dir());

	const double fps = 30.0;
	const Uint64 frame_ms = (Uint64)(1000.0 / fps);
	const int total_frames = (int)(duration_seconds * fps);
	size_t next_cue = 0;

	const Uint64 t0 = SDL_GetTicks();
	bool ok = true;
	int frame;
	for (frame = 0; frame < total_frames; frame++) {
		// Pace to wall clock -- see the block comment above. Only
		// waits when this iteration finished *ahead* of schedule
		// (the common case: offscreen rendering has no vsync to
		// wait on); never tries to "catch up" by skipping frames,
		// so a slow frame just makes the recording run a little
		// long in real time without desyncing frame numbers from
		// each other.
		const Uint64 target_ms = t0 + (Uint64)frame * frame_ms;
		const Uint64 now_ms = SDL_GetTicks();
		if (now_ms < target_ms)
			SDL_Delay((Uint32)(target_ms - now_ms));

		const double t = (SDL_GetTicks() - t0) / 1000.0;

		// Keeping the window pumped and quit-able matters here for
		// the same reason gui_update() pumps events during a scan
		// (see below): this runs for real wall-clock seconds with
		// no user interaction otherwise reaching SDL.
		SDL_Event ev;
		bool quit = false;
		while (SDL_PollEvent(&ev)) {
			ImGui_ImplSDL3_ProcessEvent(&ev);
			if (ev.type == SDL_EVENT_QUIT)
				quit = true;
		}
		if (quit)
			break;

		while (next_cue < SDL_arraysize(g_record_cues) &&
		    t >= g_record_cues[next_cue].t_seconds) {
			g_record_cues[next_cue].action();
			next_cue++;
		}
		// Continuous dolly-in (~1.5s) on gpu.cpp, then -- once the
		// TreeV switch's own pan (record_cue_switch_treev(), 1s) has
		// landed on geometry.c -- a slow revolve for the rest of the
		// recording. Each is a per-frame delta exactly like a mouse
		// drag would send input.cpp -- see the block comment above.
		if (t >= 9.8 && t < 11.3)
			camera_dolly(-2.0);
		if (t >= 13.0 && t < 18.5)
			camera_revolve(0.3, 0.0);

		fsv_animation_tick();

		imgui_new_frame();
		ui_main_draw();
		ui_dockspace_draw();
		ui_panels_draw();
		ui_dialogs_draw();
		ui_rail_draw(); // fsn-mode Task A3: camera control rail
		ui_overview_draw(); // fsn-mode Task C1: overview mini-map
		ui_legend_draw(); // fsn-mode Task A2: ages legend, by_timestamp+buckets only
		ImGui::Render();

		ImDrawData *draw_data = ImGui::GetDrawData();
		const bool empty_draw = draw_data == nullptr ||
		    draw_data->CmdListsCount == 0 ||
		    draw_data->DisplaySize.x <= 0.0f ||
		    draw_data->DisplaySize.y <= 0.0f;

		SDL_GPUCommandBuffer *cmd = gpu_record_begin(width, height);
		if (cmd == nullptr)
			continue; // skip this one frame, not the recording

		if (!empty_draw)
			ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, cmd);

		// fsn-mode Task C1: the overview mini-map, into its own
		// texture, before both passes below -- see submit_frame()'s
		// copy of this call for the ordering rules. Kept in the
		// recording loop too so a demo video of FSN mode shows the
		// same window a live session does.
		ui_overview_render();

		draw_scene();

		if (!empty_draw) {
			SDL_GPUColorTargetInfo target = {};
			target.texture = gpu_record_texture();
			target.load_op = SDL_GPU_LOADOP_LOAD;
			target.store_op = SDL_GPU_STOREOP_STORE;
			SDL_GPURenderPass *pass =
			    SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
			ImGui_ImplSDLGPU3_RenderDrawData(draw_data, cmd, pass);
			SDL_EndGPURenderPass(pass);
		}

		char path[1024];
		SDL_snprintf(path, sizeof(path), "%s/frame_%05d.bmp", outdir,
		    frame);
		if (!gpu_record_end(path)) {
			SDL_Log("fsv: --record: frame %d capture failed", frame);
			ok = false; // report it, but keep recording the rest
		}
	}

	g_recording = false;

	SDL_Log("fsv: --record: wrote %d frame(s) to \"%s\" (%dx%d @ %gfps)",
	    frame, outdir, width, height, fps);
	return ok;
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
	SDL_Log("Usage: %s [rootdir] [--discv|--mapv|--treev|--fsn] "
	    "[--screenshot FILE] [--record OUTDIR SECONDS]", argv0);
}

int
main(int argc, char **argv)
{
	const char *root_dir = ".";
	const char *screenshot_path = nullptr;
	const char *record_outdir = nullptr;
	double record_seconds = 0.0;
	// Same default as the GTK frontend (src/fsv.c).
	FsvMode initial_mode = FSV_MAPV;
	// Backs io.IniFilename below (Task 5.2 docking) -- declared here,
	// not inside the block that fills it in, so it outlives that block:
	// ImGui only stores the pointer, and main() never returns before
	// shutdown, so this scope is exactly as long as it needs to be.
	std::string imgui_ini_path;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--discv") == 0)
			initial_mode = FSV_DISCV;
		else if (strcmp(argv[i], "--mapv") == 0)
			initial_mode = FSV_MAPV;
		else if (strcmp(argv[i], "--treev") == 0)
			initial_mode = FSV_TREEV;
		else if (strcmp(argv[i], "--fsn") == 0)
			initial_mode = FSV_FSN;
		else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
			screenshot_path = argv[++i];
		else if (strcmp(argv[i], "--record") == 0 && i + 2 < argc) {
			record_outdir = argv[++i];
			record_seconds = SDL_atof(argv[++i]);
		} else if (argv[i][0] == '-') {
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

	// Same nvstore-backed pattern as color_init() just above, for the
	// fsn-mode landscape preset (Task A1): reads ~/.fsvrc's `landscape`
	// key (default "slate", today's pre-A1 look) and pushes it to
	// gpu_set_landscape() before the first frame draws.
	landscape_init();

	// fsn-mode Task C2: loads the persisted "marks" vector (named node
	// bookmarks) from ~/.fsvrc, same nvstore-backed pattern and the same
	// startup slot as color_init()/landscape_init() just above. Safe
	// before any filesystem has been scanned -- it only populates
	// name/path strings, resolving no GNode pointers until a mark is
	// drawn or gone to (src/sdl/ui_rail.cpp).
	ui_marks_init();

	// fsn-mode Task C3: loads the persisted "open_files_allowed" flag
	// (double-click-opens-a-file's "Always allow" checkbox) from
	// ~/.fsvrc. Same startup slot and nvstore-backed pattern as the
	// three calls just above.
	ui_dialogs_init();

	// Before the scan, not after: scanning a large tree takes minutes,
	// and gui_update() paints its progress overlay through these
	// backends the whole time. --screenshot skips them (and so renders
	// no progress frames) because it never opens a window loop.
	if (screenshot_path == nullptr) {
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGui::StyleColorsDark();

		// Docking (Task 5.2): the vendored ImGui is the docking branch
		// (v1.92.9b-docking, per this file's own header) and the brief
		// asks for a real "left dock", not a floating window merely
		// positioned there -- ui_panels_draw()'s "Directory Tree"
		// window becomes dockable the moment this flag is set, with no
		// other change to that file needed (any ImGui::Begin() window
		// is dockable once ImGuiConfigFlags_DockingEnable is set,
		// unless it opts out with ImGuiWindowFlags_NoDocking).
		ImGuiIO &io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

		// Panel font (post-port UTF-8 work, see docs/PORTING.md).
		// ImGui's built-in ProggyClean is a baked ASCII-only bitmap,
		// so every accented filename in the tree/file-list panels
		// came out as '?' -- the same defect the 3D labels had. Load
		// the *same* face src/fontatlas.c rasterized the label atlas
		// from, so the two never disagree about which typeface (or
		// which fallback) is in use.
		//
		// No glyph ranges: since 1.92 ImGui loads glyphs on demand
		// whenever the backend sets ImGuiBackendFlags_RendererHasTextures
		// (imgui.h: "specifying glyph ranges is only useful/necessary
		// if your backend doesn't support [it]"), which
		// imgui_impl_sdlgpu3.cpp does. So this covers all of Unicode
		// the font itself covers, not just Latin Extended-A.
		int font_index = 0;
		const char *font_path = font_atlas_find_font(&font_index);
		if (font_path != nullptr) {
			ImFontConfig font_cfg;
			font_cfg.FontNo = font_index; // face within a .ttc
			// Report an unreadable font by returning NULL rather
			// than firing IM_ASSERT_USER_ERROR: a missing/corrupt
			// system font must degrade to the default font, not
			// abort the program.
			font_cfg.Flags |= ImFontFlags_NoLoadError;
			if (io.Fonts->AddFontFromFileTTF(font_path, 15.0f, &font_cfg) == nullptr) {
				SDL_Log("fsv: could not load %s for the panels; "
				    "falling back to ImGui's ASCII-only default font",
				    font_path);
				io.Fonts->AddFontDefault();
			}
		} else {
			// font_atlas_find_font() already logged the miss once.
			io.Fonts->AddFontDefault();
		}

		// .ini persistence policy: point it at a stable, absolute path
		// instead of leaving ImGui's default ("imgui.ini", relative to
		// the process's current directory). That default would be
		// actively wrong here -- scanfs.c chdir()s into the scanned
		// root and never chdir()s back (scanfs.c:320), so a relative
		// ini would land inside whatever directory the user last
		// scanned (their home directory, a random project checkout,
		// ...), not a stable location, and Rescan/Change Root would
		// keep moving it around underneath itself. SDL_GetPrefPath()
		// is SDL3's own answer to "where can I safely write files"
		// (SDL_filesystem.h's own doc comment) -- e.g. `~/Library/
		// Application Support/fsv/fsv/` on macOS, created if needed.
		// imgui_ini_path itself is declared in main()'s own scope (see
		// above this block), not here: io.IniFilename only stores the
		// pointer, so the string backing it must outlive this `if`.
		char *pref_path = SDL_GetPrefPath("fsv", "fsv");
		if (pref_path != nullptr) {
			imgui_ini_path = pref_path;
			imgui_ini_path += "imgui.ini";
			SDL_free(pref_path);
			io.IniFilename = imgui_ini_path.c_str();
		} else {
			// No writable pref dir (unusual) -- disable persistence
			// rather than fall back to the cwd-relative default this
			// whole block exists to avoid.
			io.IniFilename = nullptr;
		}

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

	// --record: Task 6.4's demo-video capture. Unlike --screenshot, this
	// needs ImGui (the menu bar/docked panels are meant to be visible in
	// the recording) -- which is already initialized above, since this
	// branch is only reachable when screenshot_path was null and so the
	// `if (screenshot_path == nullptr)` ImGui-init block already ran.
	// Full shutdown sequence (unlike --screenshot's shorter one), because
	// there is an ImGui context here to tear down.
	if (record_outdir != nullptr) {
		bool ok = run_record_mode(record_outdir, record_seconds);

		SDL_WaitForGPUIdle(device);
		ImGui_ImplSDL3_Shutdown();
		ImGui_ImplSDLGPU3_Shutdown();
		ImGui::DestroyContext();
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

		// One hover pick per loop iteration, from the last motion
		// position seen in the drain above -- see input.h. Deliberately
		// after the minimized check (a pick is a real GPU round-trip;
		// there is nothing to highlight on a hidden window) and before
		// fsv_animation_tick(), so the highlight it sets is part of the
		// frame this iteration renders rather than the next one.
		input_flush_hover_pick();

		// fsn-mode Task B2: fsn's middle-drag flight is a velocity,
		// not a morph -- there is no end value and no duration for
		// the morph queue to interpolate toward, so it is integrated
		// here, per iteration, against real elapsed time. See the
		// "fsn flight navigation" block in src/camera.c for why that
		// is cheaper and more honest than re-arming a one-frame morph
		// every frame. A cheap no-op when nothing is flying, and it
		// runs before fsv_animation_tick() so the redraw() it asks for
		// is serviced by this same iteration rather than the next.
		//
		// Not added to run_record_mode()'s loop: that loop never calls
		// input_handle_event(), so no flight can ever be in progress
		// there (see its own block comment -- its camera motion is
		// scripted camera_dolly()/camera_revolve() calls).
		camera_flight_tick();

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
		// Order matters, in two independent ways:
		//  1. ui_main_draw() (Task 5.1: menu bar, node context menu,
		//     Help windows) before ui_dockspace_draw()/ui_panels_draw():
		//     ImGui::GetMainViewport()'s WorkPos/WorkSize is shrunk by
		//     BeginMainMenuBar()/EndMainMenuBar() *inside*
		//     ui_main_draw(), and both docking (the dockspace's own
		//     size) and the panel's fallback floating placement read
		//     that viewport rect.
		//  2. ui_dockspace_draw() (Task 5.2: docking) before
		//     ui_panels_draw(): ImGui's own docs -- "Dockspaces need to
		//     be submitted before any window they can host". Only
		//     ui_panels_draw()'s window ever docks into it; ui_main_draw()'s
		//     menu bar/popups don't, so their relative order doesn't matter.
		ui_main_draw();
		ui_dockspace_draw();
		ui_panels_draw();
		// Task 5.3: Color Setup + Properties. No ordering constraint with
		// the three calls above -- neither of ui_dialogs.cpp's windows
		// docks or reads the main-menu-bar-shrunk viewport rect the way
		// ui_panels_draw()'s panel does.
		ui_dialogs_draw();
		ui_rail_draw(); // fsn-mode Task A3: camera control rail
		// fsn-mode Task C1: the overview mini-map window. Reads the
		// main-menu-bar-shrunk viewport rect for its first-use-ever
		// top-right placement, so it belongs after ui_main_draw() --
		// same constraint as the panels above.
		ui_overview_draw();
		ui_legend_draw(); // fsn-mode Task A2: ages legend, by_timestamp+buckets only
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
