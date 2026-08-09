// src/sdl/ui_overview.h — SPDX-License-Identifier: MIT
//
// fsn-mode Task C1: the "overview" window -- upstream fsn's small
// picture-in-picture map of the whole landscape seen from above, with a
// marker at the camera's position (the window in the top-right of
// reference screenshot 3060c037-069f-4715-a01e-c30e53e505a2.jpg).
//
// Two entry points, called from two different phases of main.cpp's
// frame, because the mini-map is a texture the UI *shows* and the
// renderer *fills*:
//
//   ui_overview_draw()    UI phase, alongside ui_rail_draw() -- builds
//                         the ImGui window and handles clicks in it.
//   ui_overview_render()  frame phase, inside submit_frame(), before the
//                         visible scene pass -- re-renders the mini-map
//                         texture if anything it shows has changed.
//
// Kept common.h-free like ui_rail.h/ui_panels.h: every caller (main.cpp)
// already includes common.h itself.
#pragma once

// Builds the overview window. FSN mode only -- a top-down map of a
// pedestal landscape means nothing in DiscV/MapV/TreeV -- and, like
// ui_rail_draw(), a no-op while a scan is running, before the first
// filesystem has loaded, or while hidden via the View menu. Call once
// per frame, after ui_main_draw() (whose menu bar shrinks the main
// viewport's work area this window's first-use-ever placement reads).
//
// A click inside the map flies the camera to the nearest pedestal
// (fsn_layout_nearest() + camera_look_at()). Deliberately no
// drag-navigation: the plan calls it YAGNI until asked for.
void ui_overview_draw(void);

// Re-renders the mini-map texture if -- and only if -- something it
// shows has moved since the last render: the camera, the layout, the
// expand/collapse state, or the visualization mode. Idle frames
// therefore cost nothing beyond the comparison itself.
//
// MUST be called inside a frame (after gpu_frame_begin(), before
// gpu_frame_end()) and before the ImGui pass that samples the texture --
// see gpu_overview_render()'s contract in src/sdl/gpu_internal.hpp.
void ui_overview_render(void);

// View menu (ui_main.cpp): whether the overview window is currently
// shown. Defaults to shown; the window is only ever *drawn* in FSN mode,
// so this being true costs nothing in the other modes.
bool ui_overview_get_visible(void);
void ui_overview_set_visible(bool visible);

/* end ui_overview.h */
