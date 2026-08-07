// src/sdl/ui_main.h — SPDX-License-Identifier: MIT
//
// ImGui menu bar and mode switching for the SDL/Metal frontend. Replaces
// the GTK menu shell (src/gui.c's menu widgets, built in src/window.c's
// window_init(), with actions in src/callbacks.c).
#pragma once

// Builds this frame's menu bar, node context-menu popup and Help windows.
// Call once per frame, between main.cpp's imgui_new_frame() and
// ImGui::Render() -- exactly where the vendored ImGui example expects
// application widgets to go.
void ui_main_draw(void);
