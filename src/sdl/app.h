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

// True for the duration of --record's capture loop. That loop draws the
// real menu bar and pumps real events, but never calls
// app_apply_pending_root_change() -- so a Rescan/Change Root queued from
// it would silently never run. ui_main.cpp greys those two items out on
// this flag for the same reason it does on app_is_scanning(): a menu item
// that cannot do its job should not look like it can. Always false in the
// normal main loop.
bool app_is_recording(void);

// The directory last (successfully) passed to scanfs() -- window-title
// text, Rescan's implicit target, and the folder dialog's default
// location. Never NULL once the first scan has completed.
const char *app_root_dir(void);

// fsn-mode Task A3's camera rail (src/sdl/ui_rail.cpp) -- "Reset" button:
// re-runs the current mode's "switch into this mode" sequence in place
// (geometry_init() + camera_init(mode, FALSE) + the short "" pan) -- the
// same body app_switch_mode() runs, but without its mode ==
// globals.fsv_mode guard, which exists specifically to reject what Reset
// wants to do (re-enter the *current* mode). No-op while a scan is
// running or before the first filesystem has loaded. If bird's-eye view
// is active, this backs it out first (camera_birdseye_view(FALSE)) and
// then cancels the resulting in-flight morph with camera_pan_break() --
// NOT equivalent to the user manually exiting bird's-eye view first (that
// lets the restore morph run to completion over several seconds); see
// main.cpp's implementation comment for why the cancel is required
// before camera_init() runs.
void app_reset_camera(void);

// fsn-mode Task A3: the scroll state camera_update_scrollbars() last
// pushed for the given axis (0=x, 1=y) via fsv_platform.set_scroll() --
// verbatim lower/upper/page/value (see src/fsv-platform.h's doc comment).
// src/sdl/ui_rail.cpp's Tilt/Height sliders read this every frame to
// render a range that matches what the camera currently considers
// scrollable; not meaningful outside MapV/TreeV (see camera.c's
// null_get_scrollbar_state()/discv_get_scrollbar_state()).
void app_get_scroll_range(int axis, double *lower, double *upper,
    double *page, double *value);

// fsn-mode Task A3: called by src/sdl/ui_rail.cpp when the user drags a
// rail slider. Updates the stored scroll value (what
// fsv_platform.set_scroll() would have pushed had the camera moved on its
// own) and forwards to camera_scrollbar_moved(), the same two-step a real
// GtkAdjustment "value_changed" signal produces for GTK's
// on_scrollbar_value_changed() -- the widget updates its own value first
// (implicitly, by the drag), *then* the signal fires and reads it back
// via fsv_platform.get_scroll(). Order matters here for the same reason:
// camera_scrollbar_moved() reads the new value back through
// fsv_platform.get_scroll(), so the stored value must already be updated
// before it is called.
void app_scrollbar_dragged(int axis, double new_value);
