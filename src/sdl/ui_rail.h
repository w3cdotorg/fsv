// src/sdl/ui_rail.h — SPDX-License-Identifier: MIT
//
// fsn-mode's control rail + ages legend: the original fsn's left-hand
// button/slider column (reset/back/birdseye/front view + Tilt/Height --
// see task-A2-brief.md's "Target File Structure") and the bottom-center
// "ages:" swatch bar (task-A2-brief.md's reference screenshot,
// 35037135976_0d90f4a3d5_z.jpg). Task A2 adds just the legend; Task A3
// is expected to extend this same file with the camera rail widgets,
// per the plan.
//
// Kept common.h-free like src/sdl/ui_dialogs.h/ui_panels.h's own
// headers: every caller (main.cpp) already has its own #include
// "common.h".
#pragma once

// fsn-mode Task A3: the left-hand camera control rail -- Reset / Go back /
// Birds eye / Front view buttons plus Tilt/Height vertical sliders, wiring
// src/camera.h's camera_look_at_previous()/camera_birdseye_view()/
// camera_scrollbar_moved() (dormant in this frontend until now) the same
// way src/window.c's GTK toolbar already does. An ordinary dockable ImGui
// window (docking is enabled globally -- see main.cpp's ImGuiConfigFlags_
// DockingEnable), not force-docked into ui_panels.cpp's dockspace the way
// the Directory Tree panel is; the user can drag it wherever, same as any
// other floating ImGui window. Call once per frame, anywhere after
// ui_main_draw() (its menu bar shrinks ImGui::GetMainViewport()'s
// WorkPos/WorkSize, which this window's first-use-ever placement reads).
// No-op while a scan is running, before the first filesystem has loaded,
// or while hidden via the View menu (ui_rail_set_visible()) -- same three
// conditions ui_panels_draw() already gates the Directory Tree panel on.
void ui_rail_draw(void);

// View menu (ui_main.cpp): whether the rail is currently shown.
bool ui_rail_get_visible(void);
void ui_rail_set_visible(bool visible);

// Draws the bottom-center "ages:" legend bar -- 7 color swatches, each
// with its bucket's label underneath (src/fsn-style.h's
// fsn_age_buckets[]) -- anchored to the bottom-center of the main
// viewport, the same ImGuiViewport-anchored pattern main.cpp's own scan
// overlay uses. A silent no-op unless the live color mode is
// COLOR_BY_TIMESTAMP *and* its spectrum type is SPECTRUM_FSN_BUCKETS
// (src/color.h) -- every other color mode/spectrum combination draws
// nothing. Safe to call every frame, including before the first
// filesystem has loaded (color_init( ) has already run by main( ) by
// the time any frame is drawn).
//
// Select-pass discipline: this is a plain ImGui window, composited by
// ImGui's own SDL_GPU backend entirely outside gpu.h's scene pass
// (gpu_render_mode( )'s FSV_RENDER_SELECT concept has no meaning here)
// -- unlike the 3D scene, it is never a candidate for id-color picking
// and needs no id-color/skip decision the way new gpu.h-drawn geometry
// would.
void ui_legend_draw(void);

/* end ui_rail.h */
