// src/sdl/app.h — SPDX-License-Identifier: MIT
//
// The application-level actions Task 5.1's ImGui menu bar (src/sdl/
// ui_main.cpp) needs to reach inside src/sdl/main.cpp: mode switching and
// root-directory changes. Kept as a seam instead of duplicating main.cpp's
// load_filesystem()/enter_mode() statics -- "reuse, don't duplicate" per
// the brief.
//
// Deliberately include-free: FsvMode (src/common.h) crosses this boundary
// as a plain int, the same way src/sdl/input.h keeps GNode* as void*.
// common.h's own header guard (`#ifdef FSV_COMMON_H #error`) forbids a
// second #include within a translation unit that already has one, and
// every caller of this header (main.cpp, ui_main.cpp) already includes
// common.h itself.
#pragma once

// Vis menu: switches to a different visualization mode on the
// already-loaded filesystem. Full port of fsv.c's fsv_set_mode() for its
// "not first load" branch (geometry_init() + camera_init(mode, FALSE) +
// the short "" pan, vs. load_filesystem()'s "new_fs" 4-second fly-in,
// which stays main.cpp-private). No-op if mode == globals.fsv_mode
// (matches callbacks.c's on_vis_*_activate guard), if nothing has been
// scanned yet, or while a scan is running.
void app_switch_mode(int mode /* FsvMode */);

// File -> Change Root...: opens the native folder-picker
// (SDL_ShowOpenFolderDialog) defaulted to the current root, mirroring
// dialog_change_root(). The dialog is asynchronous and its callback may
// land on another thread (see main.cpp for the SDL_RunOnMainThread( )
// marshaling); once a folder is chosen this queues a full rescan of it,
// applied by app_apply_pending_root_change() below. No-op while a scan is
// already running.
void app_show_change_root_dialog(void);

// File -> Rescan: queues a rescan of the *current* root directory,
// applied the same deferred way. No-op while a scan is already running.
void app_request_rescan(void);

// Called once per iteration of main()'s loop, after submit_frame() has
// closed out the frame ui_main_draw() built the menu bar into. If
// app_show_change_root_dialog()'s callback or app_request_rescan() left a
// request queued, this is where scanfs() actually runs (with its own
// gui_update() progress-overlay frames). Deferred out of ui_main_draw()
// specifically: scanfs()'s gui_update() calls drive their own
// ImGui::NewFrame()/Render() pair, and ui_main_draw() runs *inside* the
// main loop's own NewFrame()/Render() pair -- calling scanfs() from
// there would re-enter ImGui::NewFrame() while it's already open.
void app_apply_pending_root_change(void);

// True while scanfs() is running (the same flag main.cpp's gui_update()
// already gates on). ui_main.cpp greys out the menu items that would
// otherwise start a second, overlapping scan.
bool app_is_scanning(void);

// The directory last (successfully) passed to scanfs() -- window-title
// text, Rescan's implicit target, and the folder dialog's default
// location. Never NULL once the first scan has completed.
const char *app_root_dir(void);
