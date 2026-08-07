// src/sdl/input.cpp — SPDX-License-Identifier: MIT
//
// Port of src/viewport.c's viewport_cb() — mouse navigation and node
// selection for the 3D viewport — onto SDL3. Every gesture below is
// traced back to a specific block of viewport.c; deviations (additions,
// simplifications, or things viewport.c does that have no SDL/ImGui
// equivalent) are called out in comments here and summarized in
// docs/PORTING.md.
//
// Double-click: viewport.c's GDK_2BUTTON_PRESS case is `/* Ignore second
// click of a double-click */ break;` -- the *first* click's ordinary
// GDK_BUTTON_PRESS has already run by the time GDK additionally reports
// the double-click, and viewport.c adds nothing on top of it. There is no
// "activate" / warp-into-directory / open-file action in the 3D viewport
// itself (that exists in dirtree.c, for the separate directory-tree pane,
// and is Task 5.2's ImGui panel to port -- out of scope here). SDL has no
// event to ignore in the first place: SDL_EVENT_MOUSE_BUTTON_DOWN fires
// once per physical press with a `clicks` count (1, 2, 3...) attached,
// rather than GDK's extra event on top. Not branching on
// `ev->button.clicks` below reproduces viewport.c's actual behavior
// exactly: a double-click is just two ordinary clicks in a row.
#include "input.h"

#include <imgui.h>

#include "gpu.h" /* gpu_pick() -- self-guards extern "C" */

extern "C" {
#include "common.h"
#include "camera.h"
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

// Context-menu request seam (Task 5.1) -- see input.h. Written only from
// the right-click branch below; read/cleared only by
// input_take_context_menu_request(). win_x/win_y are LOGICAL window
// coordinates (input.h's doc comment), never the pixel_scale()-scaled
// ones this file uses for picking -- see the fill site below.
static ContextMenuRequest g_context_menu_request = { false, nullptr, 0.0f, 0.0f };

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
		window_statusbar(SB_RIGHT, node_absname(g_indicated_node));
	}
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

		const bool ctrl_key = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
		const bool btn1 = ev->button.button == SDL_BUTTON_LEFT;
		const bool btn2 = ev->button.button == SDL_BUTTON_MIDDLE;
		const bool btn3 = ev->button.button == SDL_BUTTON_RIGHT;

		const float scale = pixel_scale(ev->button.windowID);
		const double x = ev->button.x * scale;
		const double y = ev->button.y * scale;

		if (camera_moving()) {
			// "Yipe! Impatient user" -- viewport.c's own comment.
			camera_pan_finish();
			g_indicated_node = NULL;
		} else if (!ctrl_key) {
			if (btn2)
				g_indicated_node = NULL;
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
		const bool ctrl_key = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
		const bool btn1 = ev->button.button == SDL_BUTTON_LEFT;

		if (btn1 && !ctrl_key && !camera_moving() && g_indicated_node != NULL)
			camera_look_at(g_indicated_node);

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
			if (btn2) {
				// Dolly the camera.
				const double dy = MOUSE_SENSITIVITY * (y - g_prev_y);
				camera_dolly(-dy);
				g_indicated_node = NULL;
			} else if (ctrl_key && btn1) {
				// Revolve the camera.
				const double dx = MOUSE_SENSITIVITY * (x - g_prev_x);
				const double dy = MOUSE_SENSITIVITY * (y - g_prev_y);
				camera_revolve(dx, dy);
				g_indicated_node = NULL;
			} else if (!ctrl_key && (btn1 || btn3)) {
				// Pointless dragging.
				if (g_indicated_node != NULL) {
					GNode *node = node_at_cursor((int)x, (int)y);
					if (node != g_indicated_node)
						g_indicated_node = NULL;
				}
			} else {
				g_indicated_node = node_at_cursor((int)x, (int)y);
			}

			update_highlight(btn1);
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
		break;

	default:
		break;
	}
}

ContextMenuRequest
input_take_context_menu_request(void)
{
	ContextMenuRequest req = g_context_menu_request;
	g_context_menu_request.pending = false;
	return req;
}
