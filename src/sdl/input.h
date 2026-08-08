// src/sdl/input.h — SPDX-License-Identifier: MIT
//
// Mouse navigation and node-selection input for the SDL/Metal frontend.
// Port of src/viewport.c's viewport_cb() (GTK's GdkEvent switch) onto
// SDL3's SDL_Event. The GTK arm (src/viewport.c) is untouched; this is
// the SDL-only counterpart, wired into src/sdl/main.cpp's event loop.
// Also owns one keyboard shortcut with no viewport.c counterpart at all
// (Escape-to-collapse, a post-port addition -- see input.cpp's header
// comment and docs/PORTING.md's "Post-port additions").
//
// See docs/PORTING.md's Task 4.1 section for the full GTK -> SDL gesture
// mapping table and the deviations from viewport.c's exact math.
#pragma once

#include <SDL3/SDL.h>

// Call once per polled SDL_Event, after ImGui_ImplSDL3_ProcessEvent() so
// ImGui's io.WantCaptureMouse/io.WantCaptureKeyboard already reflect this
// event. Does nothing for a mouse event that starts over an ImGui window,
// nor for the Escape keyboard shortcut while ImGui owns the keyboard or has
// a popup open. A drag already in progress (SDL_CaptureMouse()-held:
// middle-button dolly, or Ctrl+left revolve) keeps running regardless of
// what the cursor is over, mirroring how GTK's implicit pointer grab kept
// viewport.c's own drags alive past the widget's bounds.
//
// The camera side needs no per-frame tick: viewport.c only ever moves the
// camera from GDK_MOTION_NOTIFY's own delta, never from a timer or the
// busy animation loop while a button is held but the mouse is stationary.
// The *hover pick* side does -- see input_flush_hover_pick() below.
void input_handle_event(const SDL_Event *ev);

// Runs the hover pick (the "which node is under the cursor with no button
// held" highlight) that input_handle_event() deferred, at most once per
// call. Call once per main-loop iteration, after the SDL_PollEvent()
// drain and before fsv_animation_tick(), so the highlight is up to date
// for the frame this iteration renders.
//
// Why it is deferred at all: that pick is a gpu_pick(), i.e. a full
// offscreen id-colour render plus a fence wait -- 1.85ms to 9.29ms
// measured. SDL delivers one motion event per physical mouse report, so a
// single drain can hold a dozen of them, and doing the pick inline turned
// a fast mouse sweep into multiple synchronous GPU round-trips per frame.
// Only the last position is still under the cursor by the time anything
// is drawn, so coalescing to it costs no visible fidelity.
//
// The click path is NOT deferred: a press picks inline, in
// input_handle_event(), exactly as before. A press is one event, not a
// flood, and its pick has to resolve before the same event decides
// whether to open a context menu.
void input_flush_hover_pick(void);

// Drops every piece of state this file carries across events: the
// indicated (highlighted) node, any pending context-menu request, and the
// SDL_CaptureMouse() grab plus its drag origin. Call before scanfs() runs
// -- i.e. from main.cpp's load_filesystem(), alongside the core's own
// invalidation -- because the first two are GNode * into the filesystem
// tree the scan is about to free. Without it, a BUTTON_UP arriving after
// a Rescan hands the freed node straight to camera_look_at().
void input_reset(void);

// ---- Context-menu seam (Task 5.1) --------------------------------------
//
// input.cpp is the one place that already knows "the user right-clicked a
// node" -- it resolves the node under the cursor via gpu_pick(), the same
// way the left-click/hover path does. ui_main.cpp is the one place that
// knows how to draw an ImGui popup. This struct/accessor pair keeps that
// dependency one-way (input.cpp writes it, ui_main.cpp reads it, never the
// reverse), the same seam style viewport_node_for_id() used for Task 4.2.
//
// `node` is kept as `void *` (really a `GNode *`) so this header stays
// glib-free, matching input.h's own SDL-only, common.h-free style.
//
// Two coordinate spaces cross paths in this file, and mixing them up is
// an easy, silent mistake (this is the *second* pixel-vs-point trap in
// this codebase -- pixel_scale() below is the first):
//
//   - "Pixel" space: SDL mouse coordinates multiplied by pixel_scale()
//     (the window's pixel density). This is what gpu_pick() and the
//     swapchain/framebuffer want -- sdl_viewport_size() in main.cpp
//     reports pixels via SDL_GetWindowSizeInPixels(). node_at_cursor()'s
//     `x, y` locals below are in this space.
//   - "Logical"/window space: plain SDL mouse coordinates, never
//     multiplied by density. This is what ImGui wants throughout its
//     API (ImGui::SetNextWindowPos(), io.MousePos, ...) --
//     imgui_impl_sdl3.cpp feeds it raw SDL event x/y and sizes
//     io.DisplaySize from SDL_GetWindowSize() (logical), never
//     SDL_GetWindowSizeInPixels(). On a 2x Retina display the two spaces
//     differ by exactly the density factor; a value from one silently
//     misplaces a widget or misses a pick if fed into the other's API.
//
// `win_x`/`win_y` below are deliberately **logical**, i.e. the raw event
// coordinates node_at_cursor()'s caller had *before* pixel_scale() was
// applied -- because their only consumer is ui_main.cpp's
// ImGui::SetNextWindowPos(). There is no pixel-space field here: the
// pick itself already happened (input.cpp resolved `node` via the
// pixel-space `x, y` locals) by the time this struct is filled, so
// nothing downstream needs that space again.
struct ContextMenuRequest {
	bool pending;
	void *node;         // GNode*
	float win_x, win_y; // LOGICAL window coords -- see above. Not pixels.
};

// Returns the most recent right-click-on-a-node request and clears the
// pending flag. Call once per frame from ui_main_draw(); at most one
// request is ever pending (matches ImGui's own single-popup-at-a-time
// model), so an unconsumed request is simply overwritten by the next
// right-click rather than queued.
ContextMenuRequest input_take_context_menu_request(void);

// ---- Open-file request seam (fsn-mode Task C3) -------------------------
//
// FSN mode's double-click-opens-a-file gesture (upstream fsn's original
// "execute or view a file"): input.cpp is the one place that already
// resolves the double-clicked node and, in FSV_FSN mode, decides whether
// its NodeType is eligible for the system opener (see input.cpp's
// node_open_eligible()) -- the same way it already decides "directory"
// for the toggle branch just above this one. src/sdl/ui_dialogs.cpp is
// the one place that knows how to draw an ImGui modal and, once
// confirmed, call SDL_OpenURL(). Same one-way, write-here-read-there
// shape as ContextMenuRequest above: input.cpp writes, ui_dialogs.cpp
// reads/clears, never the reverse.
//
// `node` is `void *` (really `GNode *`), matching ContextMenuRequest's
// own convention -- keeps this header common.h-free. ui_dialogs.cpp
// dereferences it for exactly one frame (to snapshot the display name
// and the raw filesystem path into owned strings before the confirm
// modal's first draw) and never holds onto the pointer itself, the same
// "paths, not pointers, once anything outlives a frame" discipline
// src/sdl/ui_rail.cpp's Marks panel (Task C2) already follows.
struct OpenFileRequest {
	bool pending;
	void *node; // GNode*
};

// Returns the most recent double-click-open request and clears the
// pending flag. Call once per frame from ui_dialogs_draw(); at most one
// request is ever pending (matches input_take_context_menu_request()'s
// own model), so an unconsumed request is simply overwritten by the next
// double-click rather than queued.
OpenFileRequest input_take_open_file_request(void);
