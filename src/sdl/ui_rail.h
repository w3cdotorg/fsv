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
