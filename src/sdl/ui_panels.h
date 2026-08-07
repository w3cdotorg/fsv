// src/sdl/ui_panels.h — SPDX-License-Identifier: MIT
//
// Directory-tree + file-list ImGui panel for the SDL/Metal frontend.
// Replaces src/dirtree.c + src/filelist.c (the GTK tree/list widgets in
// window.c's left-hand paned pane) with a single ImGui window: the
// directory tree on top, the contents of whichever directory is
// currently selected below. See src/sdl/ui_panels.cpp's file header for
// the full GTK -> ImGui behavior cross-reference.
//
// This header only exposes the per-frame draw call and the View-menu
// visibility toggle ui_main.cpp (Task 5.1) wires a menu item to -- every
// other symbol this file needs (dirtree_entry_new(), filelist_show_entry(),
// etc.) is the existing core-notification contract declared in
// src/dirtree.h/src/filelist.h, which ui_panels.cpp implements for real
// in place of src/sdl/stubs.c's former no-ops.
#pragma once

// Call once per frame, after ui_main_draw() (its menu bar's
// BeginMainMenuBar()/EndMainMenuBar() shrinks ImGui::GetMainViewport()'s
// WorkPos/WorkSize for this frame, which this file's initial window
// placement relies on). No-op while a scan is running or before the
// first filesystem has finished loading -- there is no stable GNode tree
// to walk in either case.
void ui_panels_draw(void);

// View menu (ui_main.cpp): whether the panel is currently shown. GTK's
// left pane (src/window.c's hpaned_w) has no show/hide toggle at all --
// this is a deliberate addition, not a port (see docs/PORTING.md).
bool ui_panels_get_visible(void);
void ui_panels_set_visible(bool visible);
