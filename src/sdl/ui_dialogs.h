// src/sdl/ui_dialogs.h — SPDX-License-Identifier: MIT
//
// The two remaining GTK dialogs from src/dialog.c, ported as ImGui
// windows for the SDL/Metal frontend: Colors -> Setup... (color-by-
// node-type / color-by-timestamp-spectrum / color-by-wildcard-pattern
// configuration) and the per-node Properties window. Both reuse the
// same core entry points dialog.c did (color_get_config()/
// color_set_config()/color_write_config(), get_node_info()) rather than
// re-deriving any of that logic -- see ui_dialogs.cpp's file header for
// the full GTK -> ImGui cross-reference.
//
// Kept common.h-free, like src/sdl/input.h's ContextMenuRequest: every
// caller (ui_main.cpp, main.cpp) already has its own single #include
// "common.h", and common.h's own guard (`#ifdef FSV_COMMON_H #error`)
// turns a second #include within the same translation unit into a hard
// compile error rather than a silent no-op.
#pragma once

// Colors -> Setup...: opens (or, if already open, resets to a fresh
// snapshot of) the color-configuration window -- three tabs mirroring
// dialog.c's notebook pages (by node type / by date-time / by wildcard
// pattern). Safe to call repeatedly; GTK's dialog_color_setup() would
// instead stack a new independent window on every call, since
// callbacks.c never guarded against that -- this is the one place
// worth de-duplicating that, since ui_main.cpp's menu item has no
// concept of "already open" to check itself.
void ui_dialogs_open_color_setup(void);

// Context-menu Properties (dialog.c's dialog_node_properties()):
// fetches get_node_info() once right now -- NOT deferred to the next
// draw -- and opens/refreshes the single reusable Properties window
// with an owned snapshot of the result. Safe to call from inside
// ui_main_draw()'s own frame: get_file_type_desc()'s gui_update() calls
// (src/common.c) are no-ops whenever a scan isn't running (see
// src/sdl/main.cpp's gui_update()), which is always true here since
// the context menu that reaches this is itself unavailable during a
// scan.
//
// `node` is `void *` (really `GNode *`) to keep this header
// common.h-free, matching src/sdl/input.h's ContextMenuRequest.node
// convention.
void ui_dialogs_open_properties(void *node);

// Force-closes the Properties window and drops its cached node
// pointers without redrawing it first. Called once from
// src/sdl/ui_panels.cpp's dirtree_clear() (itself called at the start
// of every scanfs() run, core-side) for the same reason that function
// already resets its own g_shown_dir/g_filelist_selected: a Properties
// window left open across a Change Root/Rescan would otherwise hold a
// GNode* into a tree scanfs.c is about to tear down. The color-setup
// window needs no such call: it never stores a node pointer, only a
// copy of the (tree-independent) color configuration.
void ui_dialogs_close_properties(void);

// Draws whichever of the above are currently open. Call once per
// frame, alongside ui_main_draw()/ui_panels_draw().
void ui_dialogs_draw(void);

// If Properties or Color Setup is open, closes it (Properties first if
// both are) and returns true; otherwise a no-op returning false.
//
// Exists for src/sdl/input.cpp's Escape-to-collapse handler: neither
// window is a modal or an ImGui popup (both are plain ImGui::Begin()
// windows, per this file's header comment), so ImGui's own
// io.WantCaptureKeyboard (only true for an active widget or a *modal*)
// and IsPopupOpen() never see either of them. Without this, Escape
// would fall straight through an open, unfocused Properties/Color Setup
// window into the 3D scene underneath it. Closing here mirrors each
// window's own close path exactly (the "Close" button, the title-bar
// X) -- same next-frame teardown either way, nothing skipped.
bool ui_dialogs_handle_escape(void);

/* end ui_dialogs.h */
