// src/sdl/input.cpp — SPDX-License-Identifier: MIT
//
// Port of src/viewport.c's viewport_cb() — mouse navigation and node
// selection for the 3D viewport — onto SDL3. Every gesture below is
// traced back to a specific block of viewport.c; deviations (additions,
// simplifications, or things viewport.c does that have no SDL/ImGui
// equivalent) are called out in comments here and summarized in
// docs/PORTING.md.
//
// Double-click, upstream parity: viewport.c's GDK_2BUTTON_PRESS case is
// `/* Ignore second click of a double-click */ break;` -- the *first*
// click's ordinary GDK_BUTTON_PRESS has already run by the time GDK
// additionally reports the double-click, and viewport.c adds nothing on
// top of it. SDL has no event to ignore in the first place:
// SDL_EVENT_MOUSE_BUTTON_DOWN fires once per physical press with a
// `clicks` count (1, 2, 3...) attached, rather than GDK's extra event on
// top. Not branching on `ev->button.clicks` in the DOWN case below
// reproduces that no-op by construction.
//
// Double-click-to-expand, ADDITION -- not a port, not upstream parity:
// the BUTTON_UP case below *does* branch on `ev->button.clicks` for one
// specific case -- releasing the second click of a double-click over a
// directory toggles its collapse/expand state (colexp(), the same
// single-level toggle ui_main.cpp's context menu and ui_panels.cpp's
// tree-row arrow already use) instead of the ordinary camera_look_at().
// Requested post-port, since neither viewport.c nor this port's earlier
// tasks gave the 3D viewport any directory-activation gesture at all
// (see docs/PORTING.md's "Post-port additions"). A file or empty space
// under the second click keeps the ordinary camera_look_at() -- the
// gate only fires for a directory node.
//
// FSV_FSN mode, ADDITION on top of the addition -- fsn-mode Task C4,
// "warp-lite": the toggle above is *not* what a directory double-click
// does in FSN. Upstream fsn's own double-click ("warp") flies the camera
// down onto the pedestal and was never a toggle, so this mode's branch
// auto-expands a collapsed target (colexp(COLEXP_EXPAND), letting the
// deployment morph run *during* the fly-in) and calls camera_warp_to()
// instead of camera_look_at() -- but never collapses on a second
// double-click of an already-expanded pedestal (camera_warp_to() just
// re-centers on it, which is a visible no-op if the camera is already
// there). Collapsing an FSN directory stays reachable via Escape, the
// context menu, or the panel's tree-row arrow -- see docs/PORTING.md's
// Task C4 section. Every other mode keeps the toggle above, unchanged.
//
// Double-click-opens-a-file, ADDITION -- not a port, fsn-mode Task C3:
// upstream fsn's original "execute or view a file" gesture, ported as
// "hand it to the system's default opener" rather than a literal exec()
// of the file's own bytes -- see docs/PORTING.md's Task C3 section for
// the full security-stance writeup (delegates to LaunchServices on
// macOS / xdg-open via SDL_OpenURL() on Linux, exactly what
// double-clicking the file in Finder or a file manager would do).
// FSV_FSN mode only: the BUTTON_UP case below adds one more branch
// alongside the double-click-to-expand toggle just above it -- a
// double-click landing on a regular file or symlink
// (node_open_eligible() below decides which NodeTypes qualify;
// directories are already claimed by the toggle branch, and the
// special-file types are deliberately excluded) fills
// g_open_file_request instead of calling the ordinary
// camera_look_at(). The confirmation modal and the actual
// SDL_OpenURL() call live in src/sdl/ui_dialogs.cpp, which consumes the
// request through the same one-way seam Task 5.1's ContextMenuRequest
// already established -- see input.h.
//
// Escape-to-collapse, ADDITION -- not a port, viewport.c has no keyboard
// handling in the 3D view at all: the SDL_EVENT_KEY_DOWN case below is
// new, not a translation of any GDK_KEY_PRESS branch. Pressing Escape
// closes globals.current_node's directory if it is expanded
// (colexp(node, COLEXP_COLLAPSE_RECURSIVE), the same call double-click
// uses), or -- if the current node is a file or an already-collapsed
// directory -- steps out one level by collapsing its parent (if the
// parent is itself expanded) and flying the camera there. Repeated
// presses walk up the tree, and terminate at the real root: root_dnode's
// parent is globals.fstree, the invisible metanode created by
// scanfs.c's g_node_new() with NODE_METANODE (not NODE_DIRECTORY), so
// NODE_IS_DIR() on it is false and the "step out" branch below never
// fires past the root. Collapsing the root directory itself IS allowed
// when it is the current node and expanded -- ui_panels.cpp's tree-row
// arrow lets the root row collapse too (draw_dir_node(root_dnode) is
// the panel's own top-level call), so this matches, not overrides, that
// parity.
//
// Gated on io.WantCaptureKeyboard *and* a separate IsPopupOpen() check,
// not WantCaptureKeyboard alone: this app never sets
// ImGuiConfigFlags_NavEnableKeyboard (see main.cpp's ImGui init), and
// imgui.cpp's own io.WantCaptureKeyboard update only goes true for an
// active widget (g.ActiveId != 0) or a *modal* window -- an ordinary
// BeginPopup() like ui_main.cpp's right-click context menu sets
// neither. Confirmed by reading imgui.cpp rather than assumed: with
// NavEnableKeyboard off, ImGui's own nav-cancel Escape handling
// (NavUpdateCancelRequest(), gated on that same flag) never runs either,
// so nothing in ImGui would otherwise close that popup on Escape at
// all. ui_main.cpp's draw_context_menu() therefore has its own explicit
// IsKeyPressed(ImGuiKey_Escape) -> CloseCurrentPopup(), and this case
// checks IsPopupOpen() itself so this file's own action does not *also*
// fire underneath that popup on the same keypress.
//
// Three more gates, added on review, cover what WantCaptureKeyboard/
// IsPopupOpen() still miss:
//
//  - A right-click and this Escape landing in the *same*
//    SDL_PollEvent drain (both queued before either is processed --
//    a batched/scripted injection, or just a fast enough double
//    action) hit IsPopupOpen() before the context-menu popup exists:
//    the BUTTON_DOWN case below only fills g_context_menu_request and
//    returns, and ui_main.cpp's draw_context_menu() doesn't call
//    ImGui::OpenPopup() on it until the *next* ui_main_draw() -- one
//    full frame later than this same drain's Escape. So this case
//    also checks g_context_menu_request.pending directly (this file
//    owns that struct -- see the seam doc comment in input.h) and
//    cancels it rather than falling through to the scene: a batched
//    right-click+Escape should read the same as "open the menu, then
//    immediately close it", not "open the menu AND collapse a node".
//  - fsn-mode Task C3: a double-click-opens-a-file BUTTON_UP and this
//    Escape landing in the same drain hit exactly the same gap, for
//    exactly the same reason -- g_open_file_request is filled by the
//    BUTTON_UP case below, and ui_dialogs.cpp's draw_open_file_confirm()
//    doesn't call ImGui::OpenPopup() on it (which is what would finally
//    make WantCaptureKeyboard true) until the *next* frame. Symmetric
//    fix: this case also checks g_open_file_request.pending and cancels
//    it rather than falling through to the scene -- a batched
//    double-click+Escape should read as "the confirm almost opened,
//    then got dismissed", not "open the file AND collapse a node".
//  - Properties and Color Setup (src/sdl/ui_dialogs.cpp) are plain
//    ImGui::Begin() windows, not popups or modals -- neither
//    WantCaptureKeyboard nor IsPopupOpen() above ever sees them, even
//    while one is open and focused with no widget inside it active.
//    Without an explicit check, Escape would fall straight through an
//    open Properties/Color Setup window into the 3D scene underneath
//    it. ui_dialogs_handle_escape() closes whichever is open (see its
//    own doc comment for the fixed close order) and reports whether it
//    did, so this case can stop there instead.
#include "input.h"

#include <imgui.h>

#include "gpu.h" /* gpu_pick() -- self-guards extern "C" */
#include "ui_dialogs.h" /* ui_dialogs_handle_escape() -- Properties/Color Setup, neither a popup or modal */

extern "C" {
#include "common.h"
#include "camera.h"
#include "colexp.h" /* colexp() -- double-click-to-expand, see header comment above */
#include "dirtree.h" /* dirtree_entry_expanded() -- same toggle test ui_main.cpp's context menu uses */
#include "filelist.h" /* filelist_show_entry() */
#include "geometry.h" /* geometry_highlight_node(), geometry_should_highlight() */
#include "viewport.h" /* viewport_node_for_id() */
#include "window.h" /* window_statusbar(), SB_RIGHT */
}

// Sensitivity factor for manual camera control. Exactly viewport.c's
// MOUSE_SENSITIVITY (0.5) -- kept as its own constant here rather than a
// shared header because it belongs to the *input* side of the port, the
// same way viewport.c privately #defined it rather than putting it in
// camera.h.
#define MOUSE_SENSITIVITY 0.5

// Scroll wheel -> camera_dolly() scale. viewport.c has no scroll-wheel
// gesture at all (fsv's only dolly input historically has been the
// middle-button drag -- see doc/mouse.html); this constant exists only
// for the new SDL_EVENT_MOUSE_WHEEL case below, which the Task 4.1 brief
// asks for as an addition, not a port. Chosen so one notch (wheel.y ~
// ±1.0) reads as a comparable step to a ~40px middle-drag.
#define SCROLL_DOLLY_SCALE 20.0

// ---- State -------------------------------------------------------------
//
// File-static, exactly like viewport.c's own node_table/indicated_node/
// prev_x/prev_y statics: this file *is* the one place in the SDL frontend
// that owns "what the cursor is doing right now".

// The currently highlighted (indicated) node -- viewport.c's
// indicated_node.
static GNode *g_indicated_node = NULL;

// Previous pointer coordinates, in viewport pixels (already scaled by
// the window's pixel density -- see pixel_scale() below). viewport.c's
// prev_x/prev_y, which it keeps in the same scaled space.
static double g_prev_x = 0.0;
static double g_prev_y = 0.0;

// Whether this file currently holds an SDL_CaptureMouse() grab, and which
// button asked for it -- 0 means "no capture". viewport.c never needed
// this: GTK/X11 grants an *implicit* pointer grab to whichever widget a
// button was pressed over, so viewport.c kept getting GDK_MOTION_NOTIFY
// events even once the cursor left the GL area. SDL has no implicit grab
// tied to widget bounds (there are no widgets), so an explicit
// SDL_CaptureMouse() is this file's equivalent -- see input.h and
// docs/PORTING.md. (SDL3 also "auto-captures" while any button is held,
// per its own SDL_HINT_MOUSE_AUTO_CAPTURE default, which alone would
// already keep motion events flowing off-window; the explicit calls
// below are kept anyway, both because the Task 4.1 brief asks for them
// and because they make the intent self-documenting rather than resting
// on a hint whose default a user could flip.)
static bool g_mouse_captured = false;
static Uint8 g_capture_button = 0;

// fsn-mode Task B2: middle-drag flight. TRUE between the middle-button
// press that started a flight and whatever ended it (release, Escape,
// input_reset()). This file keeps its own flag rather than asking
// camera_flight_active() every time because the two can legitimately
// disagree for the rest of a drag: camera.c ends the flight on its own
// when something else claims the camera (a camera_look_at() from a
// left-click, the rail's Bird's Eye, a mode switch), and the middle
// button may still be physically held at that point -- this flag is what
// stops the motion handler from silently resurrecting the flight on the
// next event. It is reconciled against camera.c at the top of
// input_handle_event(); see reconcile_flight_state().
//
// g_flight_press_* is the press point the offsets are measured from, in
// the same PIXEL space as g_prev_x/g_prev_y (see pixel_scale() above).
// It is an anchor, not a running position: camera_flight_update() takes
// an absolute offset from the press point, not a per-event delta, which
// is exactly what makes the gesture a velocity control.
//
// g_flight_off_* is the last offset handed to camera_flight_update(),
// kept so that a Shift press or release can re-apply the *current*
// pointer position under the new modifier. Without it, Shift would only
// take effect on the next motion event -- so holding the pointer still
// and pressing Shift would do nothing at all until the user jiggled the
// mouse, which is precisely the situation a velocity control invites.
static bool g_flying = false;
static double g_flight_press_x = 0.0;
static double g_flight_press_y = 0.0;
static double g_flight_off_x = 0.0;
static double g_flight_off_y = 0.0;

// Deferred hover pick -- see input_flush_hover_pick() below. Coordinates
// are PIXEL space (pixel_scale()-multiplied), the space node_at_cursor()
// wants.
static bool g_hover_pending = false;
static double g_hover_x = 0.0;
static double g_hover_y = 0.0;

// Context-menu request seam (Task 5.1) -- see input.h. Written only from
// the right-click branch below; read/cleared only by
// input_take_context_menu_request(). win_x/win_y are LOGICAL window
// coordinates (input.h's doc comment), never the pixel_scale()-scaled
// ones this file uses for picking -- see the fill site below.
static ContextMenuRequest g_context_menu_request = { false, nullptr, 0.0f, 0.0f };

// Open-file request seam (fsn-mode Task C3) -- see input.h. Written only
// from the double-click branch below (FSV_FSN mode, an eligible
// NodeType); read/cleared only by input_take_open_file_request().
static OpenFileRequest g_open_file_request = { false, nullptr };

// ---- Helpers ------------------------------------------------------------

// Window-coordinate -> pixel-coordinate scale factor, the SDL/HiDPI
// equivalent of viewport.c's `scale = gtk_widget_get_scale_factor(
// gl_area_w)`. GTK hands viewport_cb() logical widget coordinates that
// need multiplying up to match the GL/Metal framebuffer's pixel grid;
// SDL3 mouse events report coordinates in the window's own coordinate
// space, which needs the same scaling to land on the pixel grid gpu_pick()
// and the swapchain both use (sdl_viewport_size() in main.cpp reports
// pixels, via SDL_GetWindowSizeInPixels()).
//
// This is the first of two pixel-vs-logical traps in this file: every
// value this function's result touches is pixel space and must never
// reach an ImGui API (which wants logical/window space throughout). The
// second is input.h's ContextMenuRequest -- read its doc comment before
// adding a new field or a new consumer of `x, y` below.
static float
pixel_scale(SDL_WindowID window_id)
{
	SDL_Window *win = SDL_GetWindowFromID(window_id);
	if (win == NULL)
		return 1.0f;
	float density = SDL_GetWindowPixelDensity(win);
	return density > 0.0f ? density : 1.0f;
}

// Port of viewport.c's node_at_location(): resolves the node visible at
// viewport pixel (x, y). ogl_select_modern() -> gpu_pick() per gpu.h's own
// mapping comment; the node-id -> GNode* table lookup that viewport.c did
// inline is now viewport_node_for_id() (src/sdl/stubs.c), since the table
// itself lives there (Task 3.3), not in this file.
//
// gpu_pick() is a real offscreen id-color readback as of Task 4.2, so this
// resolves to an actual GNode* whenever the cursor is over one. Silent,
// exactly like viewport.c's own node_at_location(): this runs on *every*
// hover motion event even with no button held (see the MOTION_NOTIFY port
// below), so logging in here would flood stdout on ordinary mouse movement.
static GNode *
node_at_cursor(int x, int y)
{
	unsigned int id = gpu_pick(x, y);
	if (id == 0)
		return NULL;
	return viewport_node_for_id(id);
}

// Port of the highlight/status-bar block that appears twice in
// viewport.c (once in the GDK_BUTTON_PRESS case, once in
// GDK_MOTION_NOTIFY) -- byte-for-byte the same branching:
//
//   if (indicated_node == NULL) { clear highlight; clear statusbar; }
//   else {
//       if (should_highlight || btn1) highlight(node, btn1);
//       else clear highlight;
//       statusbar <- node_absname(node);
//   }
static void
update_highlight(bool btn1_down)
{
	if (g_indicated_node == NULL) {
		geometry_highlight_node(NULL, FALSE);
		window_statusbar(SB_RIGHT, "");
	} else {
		if (geometry_should_highlight(g_indicated_node) || btn1_down)
			geometry_highlight_node(g_indicated_node, btn1_down ? TRUE : FALSE);
		else
			geometry_highlight_node(NULL, FALSE);
		window_statusbar(SB_RIGHT, node_absname_display(g_indicated_node));
	}
}

// fsn-mode Task C3: which NodeTypes the double-click-opens-a-file
// gesture applies to (see this file's header comment and
// docs/PORTING.md's Task C3 section for the full security-stance
// writeup). A regular file or a symlink both qualify: SDL_OpenURL()'s
// backend (LaunchServices on macOS, xdg-open on Linux) resolves a
// symlink's own path itself, exactly the way Finder double-clicking it
// would, so this file never has to chase the target manually. Every
// special-file type -- FIFO, socket, character or block device -- is
// deliberately excluded: opening one of those has an effect a regular
// file open does not (a FIFO open can block waiting for a reader/writer
// that never shows up; a device node's "contents" are hardware, not
// data to display), so those fall through to the ordinary
// camera_look_at() below instead, a silent no-op as far as this feature
// is concerned. NODE_DIRECTORY never reaches this function in practice
// (the double-click-to-expand toggle above claims it first) and
// NODE_METANODE is the invisible tree root's own parent, never a real
// pick result -- both listed anyway so the switch stays exhaustive.
// Written as a switch with every enumerator spelled out and no
// `default:`, same discipline middle_drag_is_flight() below uses: a new
// NodeType (src/common.h) then fails to compile here rather than
// silently inheriting an answer.
static bool
node_open_eligible(NodeType type)
{
	switch (type) {
	case NODE_REGFILE:
	case NODE_SYMLINK:
		return true;

	case NODE_METANODE:
	case NODE_DIRECTORY:
	case NODE_FIFO:
	case NODE_SOCKET:
	case NODE_CHARDEV:
	case NODE_BLOCKDEV:
	case NODE_UNKNOWN:
		return false;

	case NUM_NODE_TYPES:
		break; // sentinel value, never a real node's type
	}
	return false; // unreachable; keeps compilers without -Wswitch quiet
}

// Per-mode gesture dispatch -- the first in this file, and the pattern
// any later one should follow.
//
// viewport.c's gestures were all mode-agnostic: it drove camera.c's
// mode-agnostic entry points (camera_dolly(), camera_revolve()) and let
// camera.c's own per-mode switches sort out what they meant. fsn's
// flight is the first gesture that is not the same gesture in every
// mode -- middle-drag means "fly" in FSV_FSN and "dolly" everywhere
// else -- so the branch has to be here, where the button is known.
//
// Written as a switch with every enumerator spelled out and no
// `default:`, deliberately: -Wswitch then makes the next mode added to
// FsvMode a compile error in this file rather than a silently-inherited
// behavior. That is the same discipline camera.c's SWITCH_FAIL arms
// enforce at runtime, done at compile time because this one has a
// meaningful answer for every mode and so needs no failure arm.
static bool
middle_drag_is_flight(void)
{
	switch (globals.fsv_mode) {
	case FSV_FSN:
		return true;

	case FSV_DISCV:
	case FSV_MAPV:
	case FSV_TREEV:
	case FSV_SPLASH:
	case FSV_NONE:
		// Dolly, exactly as before this task.
		return false;
	}
	return false; // unreachable; keeps compilers without -Wswitch quiet
}

// Ends a flight from this file's side: drops the local flag and tells
// camera.c. Safe to call when no flight is in progress.
static void
stop_flight(void)
{
	g_flying = false;
	camera_flight_end();
}

// Pushes the last known pointer offset back into camera.c under the
// current Shift state. Called both from the motion handler (with a fresh
// offset) and from the Shift key handlers (with the cached one).
static void
apply_flight_offset(void)
{
	const bool shift_key = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
	camera_flight_update(g_flight_off_x, g_flight_off_y,
	    shift_key ? TRUE : FALSE);
}

// TRUE when this key event is a Shift press/release that a flight in
// progress should react to.
static bool
flight_shift_key(const SDL_Event *ev)
{
	return g_flying &&
	    (ev->key.key == SDLK_LSHIFT || ev->key.key == SDLK_RSHIFT);
}

// camera.c ends a flight on its own whenever something else takes the
// camera: camera_look_at_full() (a left-click on a pedestal, a tree row,
// Go Back, the rail's Look At), camera_birdseye_view() (the rail's
// Bird's Eye) and camera_init() (a mode switch, Reset, a rescan). None of
// those go through this file, so g_flying would stay true with no flight
// behind it -- and every branch gated on it would then misfire. The
// Escape handler is the visible one: it would consume the keypress on a
// do-nothing stop_flight() instead of collapsing the current directory,
// so Escape appeared dead for as long as the middle button stayed down.
//
// Reconciling once at the top of the dispatcher fixes all of those paths
// at once rather than one branch at a time. Only this direction needs
// reconciling: camera.c never *starts* a flight by itself.
static void
reconcile_flight_state(void)
{
	if (g_flying && !camera_flight_active())
		g_flying = false;
}

static void
begin_capture(Uint8 button)
{
	if (!g_mouse_captured) {
		SDL_CaptureMouse(true);
		g_mouse_captured = true;
		g_capture_button = button;
	}
}

static void
end_capture(Uint8 button)
{
	if (g_mouse_captured && g_capture_button == button) {
		SDL_CaptureMouse(false);
		g_mouse_captured = false;
		g_capture_button = 0;
	}
}

// ---- Event handling ------------------------------------------------------

void
input_handle_event(const SDL_Event *ev)
{
	ImGuiIO &io = ImGui::GetIO();

	// fsn-mode Task B2. Before anything branches on g_flying.
	reconcile_flight_state();

	switch (ev->type) {

	// Port of viewport.c's GDK_BUTTON_PRESS case. viewport.c's leading
	// `about(ABOUT_END)` check (exit the About splash on any click) and
	// its `globals.fsv_mode == FSV_SPLASH` early-out have no SDL
	// counterpart: this frontend has no About presentation or splash
	// screen at all (docs/PORTING.md, Task 3.3 deviation #1) -- both
	// stubs.c's about() always returns FALSE and fsv_mode is never
	// FSV_SPLASH here, so both checks would be permanently dead code.
	// Omitted rather than ported as inert.
	case SDL_EVENT_MOUSE_BUTTON_DOWN: {
		if (io.WantCaptureMouse)
			break; // ImGui owns this click (its own window/widget)

		// The click path picks immediately (below) and owns
		// g_indicated_node from here; a hover pick queued earlier in
		// this drain would only overwrite that decision -- and
		// update_highlight(false) after this event's
		// update_highlight(btn1) would drop the selection highlight.
		g_hover_pending = false;

		const bool ctrl_key = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
		const bool btn1 = ev->button.button == SDL_BUTTON_LEFT;
		const bool btn2 = ev->button.button == SDL_BUTTON_MIDDLE;
		const bool btn3 = ev->button.button == SDL_BUTTON_RIGHT;

		const float scale = pixel_scale(ev->button.windowID);
		const double x = ev->button.x * scale;
		const double y = ev->button.y * scale;

		// Addition, not a port: viewport.c's original condition here is
		// a plain `if (camera_moving())`, which unconditionally nulls
		// g_indicated_node below ("impatient user" -- discard this
		// click, it interrupted an in-flight pan). That is still the
		// right call for a FILE, or for empty space, or for an ordinary
		// second single-click: viewport.c already had no distinct
		// double-click behavior, and camera_look_at_full() is not a
		// no-op for a node that is already globals.current_node (it
		// calls window_set_access(FALSE) and restarts a full
		// minimum-duration pan -- 0.5s MapV, 2.0s DiscV, 1.0s TreeV,
		// camera.c's own DISCV_CAMERA_MIN_PAN_TIME et al -- with zero
		// visible motion), so re-arming BUTTON_UP's plain
		// camera_look_at() for a *second* click on the same file would
		// silently re-lock input for up to 2s. Only a genuine
		// double-click's second press *landing on a directory* should
		// skip the discard, so BUTTON_UP's toggle below has a node to
		// act on -- every visualization mode's minimum pan time
		// (camera.c's *_MIN_PAN_TIME) outlasts a physical double-click's
		// inter-click interval, so camera_moving() is unconditionally
		// still true at that second press regardless of node type.
		//
		// Peeking at the actual node under the cursor (rather than
		// deciding blind on clicks alone, as an earlier version of this
		// change did) is what tells directories and files apart here.
		// The extra node_at_cursor()/gpu_pick() call this costs is
		// bounded: it only runs on the rare BUTTON_DOWN that lands while
		// a pan is already in flight (camera_moving()), never on an
		// ordinary idle-camera press.
		GNode *impatient_peek = NULL;
		bool impatient_reclick = camera_moving();
		if (impatient_reclick && btn1 && !ctrl_key && ev->button.clicks >= 2) {
			impatient_peek = node_at_cursor((int)x, (int)y);
			if (impatient_peek != NULL && NODE_IS_DIR(impatient_peek))
				impatient_reclick = false;
			// fsn-mode Task C3: the same exemption, for the same
			// reason, extended to an FSN double-click-opens-a-file
			// target (node_open_eligible() below). Without this, a
			// real physical double-click on a file would never reach
			// the BUTTON_UP branch that opens it: this file's own
			// click==1 BUTTON_UP already called camera_look_at() and
			// restarted a full minimum-duration pan (this comment
			// block's own note, a few lines up, on why that pan runs
			// even for an already-current node) -- camera.c's
			// FSN_CAMERA_MIN_PAN_TIME (0.5s) routinely outlasts a
			// physical double-click's inter-click interval, exactly
			// like every other mode's own minimum pan time already
			// does, so camera_moving() would still read true at this
			// second BUTTON_DOWN regardless of which file was
			// clicked. Caught by tracing this function for real while
			// verifying this task, not by inspection -- a synthetic
			// double-click test with no inter-click delay would never
			// have exposed it.
			else if (impatient_peek != NULL &&
			    globals.fsv_mode == FSV_FSN &&
			    node_open_eligible(NODE_DESC(impatient_peek)->type))
				impatient_reclick = false;
		}

		if (camera_moving()) {
			// "Yipe! Impatient user" -- viewport.c's own comment. Always
			// finishes an in-progress pan on any new press, exactly as
			// before -- only whether the click is then *discarded*
			// (impatient_reclick) or allowed to pick, right below,
			// changed for the double-click-on-directory case above.
			camera_pan_finish();
		}

		if (impatient_reclick) {
			g_indicated_node = NULL;
		} else if (!ctrl_key) {
			if (btn2)
				g_indicated_node = NULL;
			else if (impatient_peek != NULL)
				g_indicated_node = impatient_peek; // reuse the peek above, don't pick twice
			else
				g_indicated_node = node_at_cursor((int)x, (int)y);

			// Left-click: "select node under cursor" (task brief) ->
			// node_at_cursor() -> gpu_pick() (Task 4.2), which now
			// resolves a real node. update_highlight() right below is
			// the actual selection behavior (highlight + status bar);
			// the fly-to itself happens on button-up, below, per
			// viewport.c's own press/release split.
			update_highlight(btn1);

			if (g_indicated_node != NULL && btn3) {
				// Right-click: viewport.c brings up context_menu()
				// (a GTK popup) here. filelist_show_entry() is called
				// for real: it is already a legitimate (if currently
				// no-op) entry point in stubs.c, and calling it now
				// needs no changes once Task 5.2 fills it in. The ImGui
				// equivalent of context_menu() itself is a one-way
				// handoff to ui_main.cpp -- see input.h's
				// ContextMenuRequest.
				filelist_show_entry(g_indicated_node);
				g_context_menu_request.pending = true;
				g_context_menu_request.node = g_indicated_node;
				// Deliberately ev->button.x/y (logical), NOT the local
				// `x, y` a few lines up (pixel_scale()-scaled, already
				// spent on node_at_cursor()'s pick above) -- see
				// input.h's ContextMenuRequest doc comment. Feeding the
				// pixel-space pair here would open the popup at up to
				// 2x its intended position on a Retina display.
				g_context_menu_request.win_x = ev->button.x;
				g_context_menu_request.win_y = ev->button.y;
			}
		}
		// ctrl_key && btn1 falls through here with no node-selection
		// side effect at all, exactly like viewport.c's `else if
		// (!ctrl_key)` -- that combo is reserved for the revolve drag
		// started below, not for picking.

		if (btn2 || (ctrl_key && btn1))
			begin_capture(ev->button.button);

		// fsn-mode Task B2: in FSV_FSN, the middle press starts a
		// flight rather than arming a dolly drag. Deliberately after
		// the camera_pan_finish() block above -- an intro pan or a
		// look_at that is still running when the user grabs the
		// controls is over, and camera_flight_begin() finishes the
		// job (camera_pan_finish() only *asks* the morphs to end on
		// the next tick, so camera_moving() is still true right here).
		if (btn2 && middle_drag_is_flight()) {
			g_flying = true;
			g_flight_press_x = x;
			g_flight_press_y = y;
			// The pointer is *at* the press point, so the offset
			// starts at zero -- not at whatever the previous
			// flight left behind, which a Shift pressed before the
			// first motion event would otherwise re-apply.
			g_flight_off_x = 0.0;
			g_flight_off_y = 0.0;
			camera_flight_begin();
		}

		g_prev_x = x;
		g_prev_y = y;
		break;
	}

	// Port of viewport.c's GDK_BUTTON_RELEASE case.
	//
	// Deviation: viewport.c reads btn1/ctrl_key back out of the
	// release event's *modifier state bitmask* (`ev_state &
	// GDK_BUTTON1_MASK`), an X11/GDK convention where that bitmask
	// still reports the button-being-released as "down". SDL's
	// per-button release event has no equivalent bitmask -- only
	// which button this release is for -- so this checks
	// `ev->button.button == SDL_BUTTON_LEFT` directly, the natural
	// reading of the same intent ("was this the left button being
	// released") without depending on the GDK quirk.
	case SDL_EVENT_MOUSE_BUTTON_UP: {
		// A release either starts a camera pan (which the hover flush
		// would skip anyway, camera_moving()) or ends a drag; neither
		// wants a stale queued hover pick applied on top.
		g_hover_pending = false;

		const bool ctrl_key = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
		const bool btn1 = ev->button.button == SDL_BUTTON_LEFT;

		if (btn1 && !ctrl_key && !camera_moving() && g_indicated_node != NULL) {
			// Addition, not a port -- see this file's header comment.
			// `g_indicated_node` is the pick already made on this
			// click's BUTTON_DOWN (the same node the plain
			// camera_look_at() below would fly to); this does not
			// re-pick. Gated on NODE_IS_DIR() the same way
			// ui_main.cpp's context menu only offers Expand/Collapse
			// for directories (colexp() itself g_asserts
			// NODE_IS_DIR()) -- a file or empty space under the
			// second click falls through to the ordinary
			// camera_look_at() below, unchanged.
			if (ev->button.clicks >= 2 && NODE_IS_DIR(g_indicated_node) &&
			    globals.fsv_mode == FSV_FSN) {
				// fsn-mode Task C4: warp-lite -- see this file's header
				// comment. dirtree_entry_expanded() is the same
				// synchronous flag the toggle branch below reads
				// (flipped the instant colexp() starts, not
				// DIR_EXPANDED()'s deployment-animation progress); only
				// a *collapsed* target gets auto-expanded here, so the
				// deployment morph runs during the fly-in rather than
				// snapping the box grid in ahead of it. An
				// already-expanded target (the re-double-click case)
				// skips straight to camera_warp_to() -- never
				// colexp(COLLAPSE): upstream fsn's warp was not a
				// toggle, and camera_warp_to() re-centering on a
				// pedestal the camera is already at is a visible no-op,
				// exactly the "smallest" re-double-click behavior the
				// task brief asks for. Collapsing stays reachable via
				// Escape, the context menu, or the panel's tree-row
				// arrow.
				if (!dirtree_entry_expanded(g_indicated_node))
					colexp(g_indicated_node, COLEXP_EXPAND);
				camera_warp_to(g_indicated_node);
			} else if (ev->button.clicks >= 2 && NODE_IS_DIR(g_indicated_node)) {
				// Same single-level toggle ui_main.cpp's context menu
				// (Expand/Collapse) and ui_panels.cpp's tree-row arrow
				// click use: dirtree_entry_expanded() is the tree
				// row's own flag (flipped synchronously the instant
				// colexp() starts), not DIR_EXPANDED()'s deployment-
				// animation progress -- see ui_main.cpp's
				// draw_context_menu() comment for why that distinction
				// matters here too. FSV_FSN is claimed by the warp-lite
				// branch just above; every other mode reaches here.
				if (dirtree_entry_expanded(g_indicated_node))
					colexp(g_indicated_node, COLEXP_COLLAPSE_RECURSIVE);
				else
					colexp(g_indicated_node, COLEXP_EXPAND);
			} else if (ev->button.clicks >= 2 &&
			    globals.fsv_mode == FSV_FSN &&
			    node_open_eligible(NODE_DESC(g_indicated_node)->type)) {
				// fsn-mode Task C3 -- see this file's header comment and
				// node_open_eligible() above. This click's own
				// BUTTON_DOWN/BUTTON_UP pair at clicks==1 already ran
				// the plain camera_look_at() branch below once (the
				// first click of any double-click always does), so the
				// camera is already there; this second release opens
				// the file instead of re-flying to a target it already
				// reached -- the same "skip the second fly" shape the
				// directory-toggle branch just above already uses.
				// ui_dialogs.cpp's draw_open_file_confirm() consumes
				// this next frame (input_take_open_file_request()).
				g_open_file_request.pending = true;
				g_open_file_request.node = g_indicated_node;
			} else {
				camera_look_at(g_indicated_node);
			}
		}

		// fsn-mode Task B2: releasing the middle button stops the
		// flight. Not gated on middle_drag_is_flight(): the mode could
		// in principle have changed since the press (a menu item is
		// still reachable with a button held), and a flight that
		// outlived its own button would never stop.
		if (ev->button.button == SDL_BUTTON_MIDDLE && g_flying)
			stop_flight();

		end_capture(ev->button.button);
		break;
	}

	// Port of viewport.c's GDK_MOTION_NOTIFY case. viewport.c also
	// guards this on `!gtk_events_pending()`, a GTK-only throttle that
	// skips processing a motion event if more are already queued
	// (avoids working from a stale coordinate under event backlog).
	// SDL_PollEvent() already delivers one event at a time with no
	// batching of its own to undo, and dropping this guard only ever
	// costs a few redundant node_at_cursor() calls under a very fast
	// drag, never correctness -- so it is not ported.
	case SDL_EVENT_MOUSE_MOTION: {
		// An in-progress drag (mouse captured) keeps running even if
		// the cursor is now over an ImGui window -- matches GTK's
		// implicit grab, which never asked GTK's widget-under-cursor
		// hit test once a drag had started.
		if (io.WantCaptureMouse && !g_mouse_captured)
			break;

		const bool ctrl_key = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
		const SDL_MouseButtonFlags state = ev->motion.state;
		const bool btn1 = (state & SDL_BUTTON_LMASK) != 0;
		const bool btn2 = (state & SDL_BUTTON_MMASK) != 0;
		const bool btn3 = (state & SDL_BUTTON_RMASK) != 0;

		const float scale = pixel_scale(ev->motion.windowID);
		const double x = ev->motion.x * scale;
		const double y = ev->motion.y * scale;

		if (!camera_moving()) {
			// Any motion supersedes a hover pick queued by an
			// earlier motion in this same drain; the branches below
			// either re-queue one (the no-button case) or take over
			// g_indicated_node themselves.
			g_hover_pending = false;

			if (btn2 && g_flying) {
				// fsn-mode Task B2: fly. Offsets are measured
				// from the press point, not from the previous
				// event -- holding the pointer still keeps
				// flying at the same speed, which is the whole
				// difference between a velocity control and a
				// drag. Shift is read live (not latched at the
				// press) so it can be pressed and released
				// mid-flight to switch between speed and
				// altitude without letting go of the button (the
				// KEY_DOWN/KEY_UP cases below re-apply the
				// cached offset, so that works even with the
				// pointer parked).
				g_flight_off_x = x - g_flight_press_x;
				g_flight_off_y = y - g_flight_press_y;
				apply_flight_offset();
				// No camera_flight_tick() here: a motion event
				// only sets the rates. The main loop integrates
				// them, so the camera keeps moving between
				// events (a held-still pointer generates none
				// at all) and the distance travelled depends on
				// elapsed time rather than on how many motion
				// events the OS happened to deliver.
				g_indicated_node = NULL;
				update_highlight(btn1);
			} else if (btn2) {
				// Dolly the camera.
				const double dy = MOUSE_SENSITIVITY * (y - g_prev_y);
				camera_dolly(-dy);
				g_indicated_node = NULL;
				update_highlight(btn1);
			} else if (ctrl_key && btn1) {
				// Revolve the camera.
				const double dx = MOUSE_SENSITIVITY * (x - g_prev_x);
				const double dy = MOUSE_SENSITIVITY * (y - g_prev_y);
				camera_revolve(dx, dy);
				g_indicated_node = NULL;
				update_highlight(btn1);
			} else if (!ctrl_key && (btn1 || btn3)) {
				// Pointless dragging. Deliberately NOT deferred:
				// this branch's meaning is "did the cursor leave
				// the node it was pressed on", which every
				// intermediate position can answer differently --
				// coalescing to the last one would miss a drag
				// that wandered off the node and back on. It also
				// only picks at all while a node is already
				// indicated, so it is not the flood the hover
				// path was.
				if (g_indicated_node != NULL) {
					GNode *node = node_at_cursor((int)x, (int)y);
					if (node != g_indicated_node)
						g_indicated_node = NULL;
				}
				update_highlight(btn1);
			} else {
				// Hover, no buttons down. This is the case that
				// used to run a full offscreen render + fence
				// wait (~2-9ms) per motion event, of which one
				// drain can deliver many. Record the position and
				// let input_flush_hover_pick() do it once, after
				// the drain -- the highlight then follows the
				// final cursor position, which is the only
				// position the user can still see.
				g_hover_pending = true;
				g_hover_x = x;
				g_hover_y = y;
			}

			g_prev_x = x;
			g_prev_y = y;
		}
		break;
	}

	// Addition, not a port: viewport.c has no scroll-wheel gesture (see
	// doc/mouse.html -- the middle-button drag is fsv's only dolly
	// input). Wired here because the Task 4.1 brief asks for it
	// explicitly. Uses the same camera_dolly() entry point and the same
	// MOUSE_SENSITIVITY-scaled-delta shape as the middle-drag case,
	// just fed from wheel ticks instead of a pixel delta. Sign: SDL
	// reports wheel.y positive "away from the user" (scroll up); that
	// dollies toward the target (dk < 0), matching the common
	// scroll-up-to-zoom-in convention.
	case SDL_EVENT_MOUSE_WHEEL: {
		if (io.WantCaptureMouse)
			break;
		if (camera_moving())
			break;
		camera_dolly(-ev->wheel.y * SCROLL_DOLLY_SCALE);
		g_indicated_node = NULL;
		g_hover_pending = false;
		break;
	}

	// Addition, not a port -- see this file's header comment. No
	// SDL_EVENT_KEY_UP counterpart: the action fires on press, exactly
	// like every other keyboard shortcut in this app (menu accelerators,
	// etc.) -- there is no press/release split here the way the mouse's
	// select-vs-fly-to gesture has one.
	// fsn-mode Task B2: Shift toggles the flight's y axis between speed
	// and altitude. The motion handler already reads SDL_GetModState()
	// live, which covers pressing Shift *while moving*; these two cases
	// cover pressing or releasing it with the pointer parked, when no
	// motion event is coming at all -- exactly the situation a velocity
	// control invites, since holding still is a legitimate way to fly.
	// Same entry point either way: the cached offset re-applied under
	// the new modifier.
	//
	// Deliberately handled ahead of the Escape case's ImGui gates. A
	// modifier arriving mid-flight is unambiguous -- flight_shift_key()
	// requires g_flying, and this file is holding a mouse capture the
	// whole time a flight is in progress -- and it consumes nothing
	// anyone else wants: no other branch in this file, and no ImGui
	// path this app enables, acts on a bare Shift. At every other
	// moment it is a no-op and the ordinary Escape handling below runs
	// untouched. SDL_GetModState() is read inside apply_flight_offset()
	// rather than derived from the event, so releasing one Shift while
	// the other is still held correctly stays "Shift down".
	case SDL_EVENT_KEY_UP:
		if (flight_shift_key(ev))
			apply_flight_offset();
		break;

	case SDL_EVENT_KEY_DOWN: {
		if (flight_shift_key(ev)) {
			apply_flight_offset();
			break;
		}

		if (ev->key.key != SDLK_ESCAPE)
			break;

		// A held key's auto-repeat KEY_DOWNs (ev->key.repeat) would
		// otherwise walk multiple levels up the tree from a single
		// physical keypress -- no other gesture in this file has an
		// auto-repeat concept to guard against (mouse clicks don't
		// repeat), so only the initial press acts.
		if (ev->key.repeat)
			break;

		// ImGui gets first refusal -- see the header comment above for
		// why this is four checks, not one.
		if (io.WantCaptureKeyboard)
			break;
		if (ImGui::IsPopupOpen("",
		    ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
			break;
		if (g_context_menu_request.pending) {
			// A right-click and this Escape landed in the same event
			// drain, before ui_main.cpp's draw_context_menu() ever got
			// a frame to turn the request into a real (IsPopupOpen()-
			// visible) popup -- see the header comment above. Cancel
			// the request instead of falling through to the scene.
			g_context_menu_request.pending = false;
			g_context_menu_request.node = nullptr;
			break;
		}
		if (g_open_file_request.pending) {
			// fsn-mode Task C3: the same same-drain race as
			// g_context_menu_request just above, mirrored -- a
			// double-click-opens-a-file BUTTON_UP and this Escape
			// landed in the same event drain, before
			// ui_dialogs.cpp's draw_open_file_confirm() ever got a
			// frame to turn the request into a real (WantCaptureKeyboard-
			// true) modal -- see the header comment above. Cancel the
			// request instead of falling through to the scene: without
			// this, the Escape would incorrectly collapse/step-out on
			// the scene below, and the confirm modal would then still
			// pop up on the very next frame regardless (input.cpp had
			// already committed the request before this key event was
			// even processed) -- exactly the unintended-collapse-plus-
			// modal-anyway race this gate closes.
			g_open_file_request.pending = false;
			g_open_file_request.node = nullptr;
			break;
		}
		if (ui_dialogs_handle_escape())
			// Properties or Color Setup was open -- see the header
			// comment above and ui_dialogs_handle_escape()'s own doc
			// comment. Consumed; leave the scene alone.
			break;

		// fsn-mode Task B2: Escape stops a flight, and does nothing
		// else on that press -- deliberately BEFORE the collapse
		// logic below, so the keystroke that pulls the user out of a
		// flight does not also close the directory they just flew
		// into. (A second press then collapses, as always.) The
		// middle button may still be held; g_flying going false is
		// what keeps the motion handler from resuming.
		if (g_flying) {
			stop_flight();
			break;
		}

		GNode *node = globals.current_node;
		if (node == NULL)
			break;

		// Same real "is this directory expanded" test the double-click
		// branch above and ui_main.cpp's context menu use:
		// dirtree_entry_expanded() (the tree row's own flag), not
		// DIR_EXPANDED() (deployment > 1-EPSILON, the *animation's*
		// progress) -- see ui_main.cpp's draw_context_menu() comment
		// for why that distinction matters.
		if (NODE_IS_DIR(node) && dirtree_entry_expanded(node)) {
			// Current node is an expanded directory: close it in
			// place. Same colexp() call the double-click branch and
			// ui_main.cpp's "Collapse" menu item use.
			colexp(node, COLEXP_COLLAPSE_RECURSIVE);
		} else {
			// Current node is a file, or an already-collapsed
			// directory: there is nothing to close *on* it, so step
			// out instead -- collapse its parent (only if the parent
			// is itself a directory that is expanded) and fly the
			// camera there, reading as "close and step out".
			// NODE_IS_DIR(parent) is false for root_dnode's own parent
			// (globals.fstree, the metanode); that is what stops this
			// from ever acting above the real root (e.g. current node
			// == an already-collapsed root_dnode falls through to a
			// no-op here, matching the panel's own
			// fully-collapsed-root state).
			GNode *parent = node->parent;
			if (parent != NULL && NODE_IS_DIR(parent) &&
			    dirtree_entry_expanded(parent)) {
				colexp(parent, COLEXP_COLLAPSE_RECURSIVE);
				// colexp() above already re-pans the camera to
				// `parent` on its own *if* the camera is not under
				// manual control (colexp.c's curnode_is_descendant
				// branch, since `node` is a descendant of `parent`)
				// -- but that auto-repan is skipped entirely whenever
				// camera->manual_control is TRUE (the user has been
				// dragging the camera manually). This explicit call
				// is what makes "Esc steps out" reliable in that case
				// too; when colexp() already panned there, this just
				// restarts the same pan to the same target -- the
				// same "double look-at, same target" pattern
				// docs/PORTING.md's Post-port additions section
				// already discloses for the double-click branch's own
				// curnode_is_equal case, not a new quirk introduced
				// here.
				camera_look_at(parent);
			}
			// Else: current node is a file/collapsed-dir whose parent
			// isn't an expanded directory (e.g. the current node
			// already IS an already-collapsed root_dnode) -- nothing
			// collapsible above it. No-op.
		}
		break;
	}

	// Port of viewport.c's GDK_LEAVE_NOTIFY case. viewport.c also resets
	// the cursor icon here (gui_cursor(gl_area_w, -1)); this frontend
	// does not swap cursor icons for any gesture (see the dolly/revolve
	// cases above, which likewise drop viewport.c's GDK_DOUBLE_ARROW/
	// GDK_FLEUR swaps) so there is nothing to reset.
	case SDL_EVENT_WINDOW_MOUSE_LEAVE:
		geometry_highlight_node(NULL, FALSE);
		window_statusbar(SB_RIGHT, "");
		g_indicated_node = NULL;
		// The cursor is gone; a hover pick queued for a position
		// inside the window would re-light the highlight this case
		// just cleared.
		g_hover_pending = false;
		break;

	default:
		break;
	}
}

void
input_flush_hover_pick(void)
{
	if (!g_hover_pending)
		return;
	g_hover_pending = false;

	// Re-checked, not inherited from the motion event: the drain may
	// have started a camera pan after the hover motion arrived (a
	// left-button release, ui_main.cpp's Look At), and viewport.c's
	// motion handler never picks while the camera is moving.
	if (camera_moving())
		return;

	g_indicated_node = node_at_cursor((int)g_hover_x, (int)g_hover_y);
	update_highlight(false); // no buttons down -- this is the hover path
}

void
input_reset(void)
{
	// The three GNode * this file can hold across frames. All of them
	// point into the tree scanfs() is about to destroy; nothing here
	// dereferences them on the way out, so this is a pure "forget".
	g_indicated_node = NULL;
	g_context_menu_request.pending = false;
	g_context_menu_request.node = nullptr;
	// fsn-mode Task C3: same reasoning as g_context_menu_request just
	// above -- a rescan is about to free the tree this points into, and
	// a BUTTON_UP arriving after it (there shouldn't be one; belt and
	// suspenders, same as the rest of this function) must not hand a
	// freed GNode* to ui_dialogs.cpp's draw_open_file_confirm() on the
	// next frame.
	g_open_file_request.pending = false;
	g_open_file_request.node = nullptr;
	g_hover_pending = false;

	// Drag/capture state. A scan blocks the main thread for as long as
	// it takes (gui_update() pumps events, but input_handle_event() is
	// not on that path), so any button held when the scan started is
	// very likely released by the time it finishes -- and the release
	// event, if it arrives at all, arrives with no matching press.
	// Dropping the grab here keeps SDL_CaptureMouse() balanced rather
	// than leaving the pointer captured for the rest of the session.
	if (g_mouse_captured)
		SDL_CaptureMouse(false);
	g_mouse_captured = false;
	g_capture_button = 0;
	g_prev_x = 0.0;
	g_prev_y = 0.0;

	// fsn-mode Task B2: same reasoning as the capture grab above -- a
	// middle button held when the scan started is very likely released
	// (with no event delivered) by the time it finishes, and a flight
	// nothing can stop would keep the camera moving through a landscape
	// that is being relaid out underneath it. camera_init() ends it too
	// once the new tree is up, but that runs later and only in the
	// paths that re-pose the camera; this is the unconditional one.
	stop_flight();
	g_flight_press_x = 0.0;
	g_flight_press_y = 0.0;
	g_flight_off_x = 0.0;
	g_flight_off_y = 0.0;
}

ContextMenuRequest
input_take_context_menu_request(void)
{
	ContextMenuRequest req = g_context_menu_request;
	g_context_menu_request.pending = false;
	return req;
}

OpenFileRequest
input_take_open_file_request(void)
{
	OpenFileRequest req = g_open_file_request;
	g_open_file_request.pending = false;
	return req;
}
