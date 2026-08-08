// src/sdl/ui_main.cpp — SPDX-License-Identifier: MIT
//
// ImGui menu bar, node context menu and Help windows for the SDL/Metal
// frontend. Replaces the GTK menu shell -- src/gui.c's menu-widget
// helpers, wired up in src/window.c's window_init(), with the actions
// themselves in src/callbacks.c -- with the same action set, adapted
// where GTK-specific (a native folder dialog instead of GtkFileChooser,
// an ImGui popup instead of a GtkMenu). Mode switching and the
// deferred-rescan machinery live in src/sdl/app.h (implemented in
// main.cpp); this file only builds widgets and calls through to them, so
// as not to duplicate main.cpp's load_filesystem()/enter_mode().
//
// Menu -> GTK original cross-reference (src/window.c's window_init(),
// src/callbacks.c):
//
//   File   -> Change Root...  == on_file_change_root_activate() -> dialog_change_root()
//          -> Rescan          == addition (see app.h); GTK has no separate "just
//                                 rescan" action, only Change Root
//          -> Quit            == on_file_exit_activate() -> exit(EXIT_SUCCESS)
//   Vis    -> DiscV/MapV/TreeV == on_vis_*_activate() -> fsv_set_mode()
//   View   -> Directory Tree && Files == addition (Task 5.2, src/sdl/ui_panels.cpp);
//                                 GTK's left pane has no show/hide toggle at all
//   Colors -> By node type/timestamp/wildcards == on_color_by_*_activate() -> color_set_mode()
//          -> Setup...        == on_color_setup_activate() -> dialog_color_setup()
//                                 (Task 5.3, src/sdl/ui_dialogs.cpp)
//   Help   -> Controls        == addition; doc/mouse.html has no GTK menu entry point
//          -> About fsv...    == on_help_about_fsv_activate() -> about(ABOUT_BEGIN)
//
// Context menu's "Properties..." == properties_cb() -> dialog_node_properties()
//   (Task 5.3, src/sdl/ui_dialogs.cpp)
#include "ui_main.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include "app.h"
#include "input.h"
#include "ui_dialogs.h"
#include "ui_panels.h"

extern "C" {
#include "common.h"
#include "camera.h"
#include "color.h"
#include "colexp.h"
#include "dirtree.h"
}

// ---- Help: About ---------------------------------------------------------

static void
draw_about_window(bool *open)
{
	if (!*open)
		return;

	ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("About fsv", open, ImGuiWindowFlags_NoResize)) {
		ImGui::Text("fsv - 3D File System Visualizer");
		ImGui::Text("Version %s", VERSION);
		ImGui::Separator();
		ImGui::TextWrapped(
		    "Copyright (C)1999 Daniel Richard G. <skunk@mit.edu>");
		ImGui::TextWrapped("Copyright (C) 2021 Janne Blomqvist");
		ImGui::Spacing();
		ImGui::TextWrapped(
		    "macOS/Metal port: SDL3 + SDL_GPU + Dear ImGui, replacing "
		    "the original GTK+3/OpenGL frontend.");
	}
	ImGui::End();
}

// ---- Help: Controls -------------------------------------------------------
//
// The real, verified gesture table from docs/PORTING.md's Task 4.1
// section -- src/sdl/input.cpp's actual behavior, not the brief's
// original (wrong) guess at it.

static void
draw_controls_window(bool *open)
{
	if (!*open)
		return;

	ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Controls", open, ImGuiWindowFlags_NoResize)) {
		if (ImGui::BeginTable("gestures", 2,
		    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
			ImGui::TableSetupColumn("Gesture");
			ImGui::TableSetupColumn("Action");
			ImGui::TableHeadersRow();

			static const char *const rows[][2] = {
				{ "Middle-button drag", "Dolly camera (zoom)" },
				{ "Scroll wheel", "Dolly camera (zoom)" },
				{ "Ctrl + left-button drag", "Revolve camera" },
				{ "Left click (press)", "Select node under cursor" },
				{ "Left click (release)", "Fly camera to selected node" },
				{ "Double-click a directory", "Toggle expand/collapse (addition -- see docs/PORTING.md)" },
				{ "Right click", "Context menu for node under cursor" },
				{ "Escape", "Collapse current directory, or step out and collapse its parent (addition -- see docs/PORTING.md)" },
			};
			for (const auto &row : rows) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(row[0]);
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(row[1]);
			}
			ImGui::EndTable();
		}
	}
	ImGui::End();
}

// ---- Node context menu ---------------------------------------------------
//
// Consumes src/sdl/input.cpp's ContextMenuRequest seam. g_context_menu_
// node stays valid only across the frames the popup itself is open --
// same lifetime assumption update_highlight()'s g_indicated_node in
// input.cpp already makes about nodes named by the current scan.

static void *g_context_menu_node = nullptr; // GNode*; void* per input.h

static void
draw_context_menu(void)
{
	ContextMenuRequest req = input_take_context_menu_request();
	if (req.pending) {
		g_context_menu_node = req.node;
		// req.win_x/win_y are logical window coordinates (input.h's doc
		// comment) -- exactly the space ImGui::SetNextWindowPos() wants.
		// Do not substitute a pixel_scale()-scaled value here: ImGui's
		// own SDL3 backend never density-scales (imgui_impl_sdl3.cpp
		// sizes io.DisplaySize from SDL_GetWindowSize(), not
		// SDL_GetWindowSizeInPixels()), so a pixel-space coordinate
		// would open this popup at up to 2x its intended position on a
		// Retina display.
		ImGui::SetNextWindowPos(ImVec2(req.win_x, req.win_y));
		ImGui::OpenPopup("node_context_menu");
	}

	if (ImGui::BeginPopup("node_context_menu")) {
		// Addition, not upstream ImGui behavior here: this app never sets
		// ImGuiConfigFlags_NavEnableKeyboard (see main.cpp's ImGui init),
		// and ImGui's own nav-cancel Escape handling that would otherwise
		// close this popup (imgui.cpp's NavUpdateCancelRequest()) is
		// itself gated on that same flag -- confirmed by reading
		// imgui.cpp, not assumed. Without this, Escape would silently do
		// nothing to this popup at all. IsKeyPressed() (unlike
		// io.WantCaptureKeyboard) works regardless of NavEnableKeyboard,
		// so this closes the popup explicitly instead. src/sdl/input.cpp's
		// own Escape-to-collapse addition checks IsPopupOpen() before
		// acting, so the two never both fire for the same keypress.
		if (ImGui::IsKeyPressed(ImGuiKey_Escape))
			ImGui::CloseCurrentPopup();

		GNode *node = static_cast<GNode *>(g_context_menu_node);
		if (node != nullptr) {
			ImGui::TextDisabled("%s", node_absname(node));
			ImGui::Separator();

			if (ImGui::MenuItem("Look At"))
				camera_look_at(node);

			if (ImGui::MenuItem("Properties..."))
				ui_dialogs_open_properties(node);

			// Same single-level toggle src/dirtree.c's collapse/expand
			// tree-view callbacks use (dirtree_collapse_cb() ->
			// COLEXP_COLLAPSE_RECURSIVE, dirtree_expand_cb() ->
			// COLEXP_EXPAND) -- colexp() asserts NODE_IS_DIR(dnode), so
			// this is only offered for directories. Label decision uses
			// dirtree_entry_expanded() (the *tree row's* state, flipped
			// synchronously the instant colexp() starts), exactly like
			// src/dialog.c's real context_menu() -- not DIR_EXPANDED()
			// (deployment > 1-EPSILON, the *animation's* progress),
			// which src/sdl/ui_panels.cpp (Task 5.2) is what makes a
			// meaningful distinction now that dirtree_entry_expanded()
			// is a real per-row flag instead of a stub.
			if (NODE_IS_DIR(node)) {
				ImGui::Separator();
				if (dirtree_entry_expanded(node)) {
					if (ImGui::MenuItem("Collapse"))
						colexp(node, COLEXP_COLLAPSE_RECURSIVE);
				} else {
					if (ImGui::MenuItem("Expand"))
						colexp(node, COLEXP_EXPAND);
				}
			}
		}
		ImGui::EndPopup();
	} else {
		// Popup closed (or never opened this frame): don't hold onto a
		// node pointer a rescan could later free.
		g_context_menu_node = nullptr;
	}
}

// ---- Menu bar -------------------------------------------------------------

void
ui_main_draw(void)
{
	static bool show_about = false;
	static bool show_controls = false;

	const bool scanning = app_is_scanning();
	// Change Root.../Rescan are additionally unavailable during --record:
	// the recording loop never applies a queued root change, so leaving
	// them enabled there would offer an action that silently does
	// nothing. See app.h's app_is_recording().
	const bool root_change_ok = !scanning && !app_is_recording();
	const FsvMode mode = globals.fsv_mode;
	// Vis menu items need real geometry to switch onto; mode is FSV_NONE
	// only during the brief window between load_filesystem()'s "wipe"
	// and its enter_mode() call, which never overlaps a drawn frame in
	// practice, but the guard costs nothing and documents the invariant.
	const bool vis_ready = !scanning && mode != FSV_NONE;

	if (ImGui::BeginMainMenuBar()) {
		if (ImGui::BeginMenu("File")) {
			if (ImGui::MenuItem("Change Root...", nullptr, false, root_change_ok))
				app_show_change_root_dialog();
			if (ImGui::MenuItem("Rescan", nullptr, false, root_change_ok))
				app_request_rescan();
			ImGui::Separator();
			if (ImGui::MenuItem("Quit")) {
				// Route through the real event queue rather than
				// calling exit() directly (what callbacks.c's
				// on_file_exit_activate() does) -- main.cpp's loop
				// already handles SDL_EVENT_QUIT correctly, including
				// the g_quit_requested path for a quit arriving mid-
				// scan, and a plain exit() here would skip
				// gpu_shutdown()/SDL_Quit().
				SDL_Event quit_ev = {};
				quit_ev.type = SDL_EVENT_QUIT;
				SDL_PushEvent(&quit_ev);
			}
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Vis", vis_ready)) {
			if (ImGui::MenuItem("DiscV", nullptr, mode == FSV_DISCV))
				app_switch_mode(FSV_DISCV);
			if (ImGui::MenuItem("MapV", nullptr, mode == FSV_MAPV))
				app_switch_mode(FSV_MAPV);
			if (ImGui::MenuItem("TreeV", nullptr, mode == FSV_TREEV))
				app_switch_mode(FSV_TREEV);
			ImGui::EndMenu();
		}

		// Addition, not a port: src/window.c's left pane (the
		// dirtree/filelist panel this toggles) has no show/hide menu
		// entry in GTK at all, only a paned-widget divider the user
		// can drag but never fully hide (docs/PORTING.md).
		if (ImGui::BeginMenu("View")) {
			const bool panels_visible = ui_panels_get_visible();
			if (ImGui::MenuItem("Directory Tree && Files", nullptr,
			    panels_visible))
				ui_panels_set_visible(!panels_visible);
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Colors")) {
			const ColorMode cmode = color_get_mode();
			if (ImGui::MenuItem("By node type", nullptr,
			    cmode == COLOR_BY_NODETYPE))
				color_set_mode(COLOR_BY_NODETYPE);
			if (ImGui::MenuItem("By timestamp", nullptr,
			    cmode == COLOR_BY_TIMESTAMP))
				color_set_mode(COLOR_BY_TIMESTAMP);
			if (ImGui::MenuItem("By wildcards", nullptr,
			    cmode == COLOR_BY_WPATTERN))
				color_set_mode(COLOR_BY_WPATTERN);
			ImGui::Separator();
			if (ImGui::MenuItem("Setup..."))
				ui_dialogs_open_color_setup();
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Help")) {
			if (ImGui::MenuItem("Controls", nullptr, show_controls))
				show_controls = !show_controls;
			ImGui::Separator();
			if (ImGui::MenuItem("About fsv..."))
				show_about = true;
			ImGui::EndMenu();
		}

		ImGui::EndMainMenuBar();
	}

	draw_about_window(&show_about);
	draw_controls_window(&show_controls);
	draw_context_menu();
}
