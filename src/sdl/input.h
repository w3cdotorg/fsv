// src/sdl/input.h — SPDX-License-Identifier: MIT
//
// Mouse navigation and node-selection input for the SDL/Metal frontend.
// Port of src/viewport.c's viewport_cb() (GTK's GdkEvent switch) onto
// SDL3's SDL_Event. The GTK arm (src/viewport.c) is untouched; this is
// the SDL-only counterpart, wired into src/sdl/main.cpp's event loop.
//
// See docs/PORTING.md's Task 4.1 section for the full GTK -> SDL gesture
// mapping table and the deviations from viewport.c's exact math.
#pragma once

#include <SDL3/SDL.h>

// Call once per polled SDL_Event, after ImGui_ImplSDL3_ProcessEvent() so
// ImGui's io.WantCaptureMouse already reflects this event. Does nothing
// for a mouse event that starts over an ImGui window; a drag already in
// progress (SDL_CaptureMouse()-held: middle-button dolly, or Ctrl+left
// revolve) keeps running regardless of what the cursor is over, mirroring
// how GTK's implicit pointer grab kept viewport.c's own drags alive past
// the widget's bounds.
//
// No per-frame tick is needed: viewport.c only ever moves the camera from
// GDK_MOTION_NOTIFY's own delta, never from a timer or the busy animation
// loop while a button is held but the mouse is stationary. This function
// is the complete port of that mechanism.
void input_handle_event(const SDL_Event *ev);

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
