// src/sdl/ui_dialogs.cpp — SPDX-License-Identifier: MIT
//
// Color Setup and Properties windows for the SDL/Metal frontend --
// Task 5.3, the last piece of GTK -> ImGui UI parity (Milestone 5).
// Ports src/dialog.c's LOGIC, not its GtkNotebook/GtkCList widgets: both
// windows below drive the same core entry points dialog.c did
// (color_get_config()/color_set_config()/color_write_config(),
// get_node_info()), just through ImGui widgets instead of GTK ones.
//
// GTK -> ImGui cross-reference (src/dialog.c):
//
//   dialog_color_setup()                  -> ui_dialogs_open_color_setup() +
//                                             draw_color_setup_window()
//   csdialog.color_config (scratch copy)  -> g_cs.scratch (same
//                                             color_get_config()/
//                                             color_config_destroy() contract)
//   "By node type" notebook page          -> draw_color_setup_nodetype_tab()
//   "By date/time" notebook page +
//     gui_spectrum_fill()                 -> draw_color_setup_timestamp_tab() +
//                                             draw_spectrum_preview()
//   "By wildcards" notebook page +
//     csdialog_wpattern_*()                -> draw_color_setup_wpattern_tab()
//   csdialog_ok_button_cb()               -> the "Apply" button in
//                                             draw_color_setup_window()
//   dialog_node_properties()              -> ui_dialogs_open_properties() +
//                                             draw_properties_window()
//   look_at_target_node_cb()              -> the "Look at target node"
//                                             button in draw_properties_window()
//
// Neither window is modal (ImGui::Begin(), not a true modal popup),
// matching every other window this frontend has added so far (About,
// Controls, the dirtree/filelist panel) -- dialog.c's GTK windows were
// modal (gui_window_modalize()) only because GTK's own idiom expects
// that; this port's own idiom, established since Task 5.1, does not.
#include "ui_dialogs.h"

#include <array>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

extern "C" {
#include "common.h"
#include "camera.h"
#include "colexp.h"
#include "color.h"
#include "dirtree.h"
}

namespace {

const char *
node_type_label(NodeType t)
{
	// node_type_names[] (src/common.c) already exists for exactly this
	// -- reuse it rather than re-deriving labels, same as dialog.c's own
	// sprintf(strbuf, _("Color: %s"), node_type_names[i]).
	return node_type_names[t];
}

// ============================================================
// Colors -> Setup...
// ============================================================

struct ColorSetupState {
	bool open = false;
	// Whether `scratch` currently holds a live color_get_config() copy
	// that owns allocations (by_wpattern.wpgroup_list and its patterns)
	// needing color_config_destroy() -- dialog.c's own csdialog_destroy_cb()
	// comment: "We'd leak memory like crazy if we didn't do this".
	bool have_scratch = false;
	ColorConfig scratch{};
	ColorMode active_mode = COLOR_BY_NODETYPE;
	// "By date/time" tab: slider-friendly mirror of
	// scratch.by_timestamp.{old,new}_time, since ImGui has no date-edit
	// widget to match dialog.c's GtkDateEdit pair.
	float old_days_ago = 7.0f;
	float new_days_ago = 0.0f;
};

ColorSetupState g_cs;

// Per-wildcard-group "add a pattern" text buffers. Keyed by the live
// WPatternGroup* (stable for as long as that group survives in
// g_cs.scratch), so each group's input row keeps its own in-progress
// text independently -- a single shared static buffer would make every
// group's row echo whatever was last typed into any of them. Cleared on
// every ui_dialogs_open_color_setup() (a fresh scratch means every old
// group pointer is already gone).
std::map<void *, std::array<char, 128>> g_new_pattern_text;

void
cs_reset_time_sliders_from_scratch(void)
{
	time_t now = time(nullptr);
	double old_secs = difftime(now, g_cs.scratch.by_timestamp.old_time);
	double new_secs = difftime(now, g_cs.scratch.by_timestamp.new_time);
	g_cs.old_days_ago = (float)(old_secs / 86400.0);
	g_cs.new_days_ago = (float)(new_secs / 86400.0);
	if (g_cs.old_days_ago < 0.0f)
		g_cs.old_days_ago = 0.0f;
	if (g_cs.new_days_ago < 0.0f)
		g_cs.new_days_ago = 0.0f;
}

// Port of dialog.c's csdialog_time_edit_cb(): keeps old_time strictly
// before new_time. That function enforced an exact 60-second minimum
// gap on a fine-grained date-time widget; the day-granularity sliders
// here instead just clamp the *ordering*, which is the part that
// actually matters to color.c's time_color()/generate_spectrum_colors()
// (difftime() of a zero-or-negative span would divide by zero or
// invert the spectrum).
void
cs_apply_time_sliders_to_scratch(void)
{
	if (g_cs.new_days_ago > g_cs.old_days_ago)
		g_cs.new_days_ago = g_cs.old_days_ago;
	time_t now = time(nullptr);
	g_cs.scratch.by_timestamp.old_time = now - (time_t)(g_cs.old_days_ago * 86400.0f);
	g_cs.scratch.by_timestamp.new_time = now - (time_t)(g_cs.new_days_ago * 86400.0f);
}

void
draw_color_setup_nodetype_tab(void)
{
	ImGui::TextUnformatted("Color assigned to each node type:");
	ImGui::Spacing();
	for (int i = 1; i < NUM_NODE_TYPES; i++) {
		RGBcolor &color = g_cs.scratch.by_nodetype.colors[i];
		ImGui::PushID(i);
		ImGui::ColorEdit3("##color", &color.r);
		ImGui::SameLine();
		ImGui::TextUnformatted(node_type_label((NodeType)i));
		ImGui::PopID();
	}
}

// Port of dialog.c's csdialog_time_spectrum_func(): same
// color_spectrum_color() call, same gradient boundary-color plumbing.
RGBcolor
cs_spectrum_sample(double x)
{
	RGBcolor *boundary[2];
	void *data = nullptr;
	if (g_cs.scratch.by_timestamp.spectrum_type == SPECTRUM_GRADIENT) {
		boundary[0] = &g_cs.scratch.by_timestamp.old_color;
		boundary[1] = &g_cs.scratch.by_timestamp.new_color;
		data = boundary;
	}
	return color_spectrum_color(g_cs.scratch.by_timestamp.spectrum_type, x, data);
}

// Cheap ImDrawList preview of the same function dialog.c's
// gui_spectrum_fill(csdialog_time_spectrum_func) painted into its
// GtkDrawingArea -- 32 stops here rather than color.c's full
// SPECTRUM_NUM_SHADES (1024): this only has to look smooth, not double
// as the live per-pixel table color_assign_recursive() reads (that
// stays color.c's own generate_spectrum_colors(), untouched).
void
draw_spectrum_preview(void)
{
	const int kStops = 32;
	const float h = 28.0f;
	const ImVec2 p0 = ImGui::GetCursorScreenPos();
	const float w = ImGui::GetContentRegionAvail().x;
	ImDrawList *dl = ImGui::GetWindowDrawList();
	for (int i = 0; i < kStops; i++) {
		const double x0 = (double)i / (double)kStops;
		const double x1 = (double)(i + 1) / (double)kStops;
		const RGBcolor c0 = cs_spectrum_sample(x0);
		const RGBcolor c1 = cs_spectrum_sample(x1);
		const ImU32 col0 = ImGui::ColorConvertFloat4ToU32(ImVec4(c0.r, c0.g, c0.b, 1.0f));
		const ImU32 col1 = ImGui::ColorConvertFloat4ToU32(ImVec4(c1.r, c1.g, c1.b, 1.0f));
		const ImVec2 a(p0.x + w * (float)x0, p0.y);
		const ImVec2 b(p0.x + w * (float)x1, p0.y + h);
		dl->AddRectFilledMultiColor(a, b, col0, col1, col1, col0);
	}
	ImGui::Dummy(ImVec2(w, h));
}

void
draw_color_setup_timestamp_tab(void)
{
	ImGui::SliderFloat("Oldest (days ago)", &g_cs.old_days_ago, 0.0f, 3650.0f, "%.1f days");
	ImGui::SliderFloat("Newest (days ago)", &g_cs.new_days_ago, 0.0f, 3650.0f, "%.1f days");
	cs_apply_time_sliders_to_scratch();

	static const char *timestamp_labels[] = {
		"Time of last access", "Time of last modification",
		"Time of last attribute change"
	};
	int ts = (int)g_cs.scratch.by_timestamp.timestamp_type;
	if (ImGui::Combo("Color by", &ts, timestamp_labels, 3))
		g_cs.scratch.by_timestamp.timestamp_type = (TimeStampType)ts;

	ImGui::Spacing();
	draw_spectrum_preview();
	ImGui::Spacing();

	static const char *spectrum_labels[] = { "Rainbow", "Heat", "Gradient" };
	int sp = (int)g_cs.scratch.by_timestamp.spectrum_type;
	if (ImGui::Combo("Spectrum type", &sp, spectrum_labels, 3))
		g_cs.scratch.by_timestamp.spectrum_type = (SpectrumType)sp;

	// Port of csdialog_time_color_picker_set_access(): the gradient
	// endpoint colors only mean anything for SPECTRUM_GRADIENT.
	const bool gradient = g_cs.scratch.by_timestamp.spectrum_type == SPECTRUM_GRADIENT;
	ImGui::BeginDisabled(!gradient);
	ImGui::ColorEdit3("Older color", &g_cs.scratch.by_timestamp.old_color.r);
	ImGui::ColorEdit3("Newer color", &g_cs.scratch.by_timestamp.new_color.r);
	ImGui::EndDisabled();
}

void
draw_color_setup_wpattern_tab(void)
{
	auto &wp = g_cs.scratch.by_wpattern; // ColorConfig::ColorByWPattern is a nested type -- `auto` sidesteps naming it

	if (ImGui::Button("New color group")) {
		// Port of csdialog_wpattern_button_cb()'s "New color" branch --
		// simplified to append at the end always (dialog.c additionally
		// offers inserting before an already-selected group's position,
		// which has no equivalent here since this tab has no
		// single-selected-row concept; YAGNI per the brief).
		auto *ng = NEW(struct WPatternGroup);
		ng->color = RGBcolor{ 0.0f, 0.0f, 0.75f }; // dialog.c's default_new_color, "I like blue"
		ng->wp_list = nullptr;
		G_LIST_APPEND(wp.wpgroup_list, ng);
	}
	ImGui::TextWrapped(
	    "Files are colored by the first pattern group they match "
	    "(checked top to bottom); unmatched files get the default "
	    "color at the bottom.");

	GList *group_to_delete = nullptr;
	for (GList *l = wp.wpgroup_list; l != nullptr; l = l->next) {
		auto *g = (struct WPatternGroup *)l->data;
		ImGui::PushID(g);
		ImGui::Separator();

		ImGui::ColorEdit3("Group color", &g->color.r);

		char *pattern_to_remove = nullptr;
		for (GList *wl = g->wp_list; wl != nullptr; wl = wl->next) {
			char *pat = (char *)wl->data;
			ImGui::PushID(wl);
			ImGui::Bullet();
			ImGui::SameLine();
			ImGui::TextUnformatted(pat);
			ImGui::SameLine();
			if (ImGui::SmallButton("Remove"))
				pattern_to_remove = pat; // deleted after the loop -- see below
			ImGui::PopID();
		}
		if (pattern_to_remove != nullptr) {
			// Port of csdialog_wpattern_button_cb()'s "Delete" branch
			// for a WPLIST_WPATTERN_ROW.
			G_LIST_REMOVE(g->wp_list, pattern_to_remove);
			xfree(pattern_to_remove);
		}

		std::array<char, 128> &buf = g_new_pattern_text[g]; // default-inits (zeroed) the first time this group is seen
		ImGui::SetNextItemWidth(200.0f);
		const bool enter = ImGui::InputText("##newpattern", buf.data(),
		    buf.size(), ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine();
		if (ImGui::SmallButton("Add pattern") || enter) {
			// Port of csdialog_wpattern_edit_cb(): trim, skip empty
			// input, skip a pattern already present in this group.
			char *candidate = xstrstrip(xstrdup(buf.data()));
			if (strlen(candidate) == 0 ||
			    g_list_find_custom(g->wp_list, candidate, (GCompareFunc)strcmp) != nullptr)
				xfree(candidate);
			else
				G_LIST_APPEND(g->wp_list, candidate);
			buf[0] = '\0';
		}

		// Port of csdialog_wpattern_list_select_unselect_cb()'s
		// delete_allow rule for a WPLIST_NEW_WPATTERN_ROW: "Delete color
		// group ONLY if group is empty".
		const bool group_empty = (g->wp_list == nullptr);
		ImGui::BeginDisabled(!group_empty);
		if (ImGui::SmallButton("Delete empty group"))
			group_to_delete = l;
		ImGui::EndDisabled();
		if (!group_empty) {
			ImGui::SameLine();
			ImGui::TextDisabled("(remove all its patterns first)");
		}

		ImGui::PopID();
	}
	if (group_to_delete != nullptr) {
		auto *g = (struct WPatternGroup *)group_to_delete->data;
		g_new_pattern_text.erase(g);
		G_LIST_REMOVE(wp.wpgroup_list, g);
		xfree(g);
	}

	ImGui::Separator();
	ImGui::ColorEdit3("Default color", &wp.default_color.r);
}

void
draw_color_setup_window(void)
{
	if (g_cs.open) {
		ImGui::SetNextWindowSize(ImVec2(460, 460), ImGuiCond_FirstUseEver);
		if (ImGui::Begin("Color Setup", &g_cs.open)) {
			if (ImGui::BeginTabBar("color_setup_tabs")) {
				if (ImGui::BeginTabItem("By node type")) {
					g_cs.active_mode = COLOR_BY_NODETYPE;
					draw_color_setup_nodetype_tab();
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("By date/time")) {
					g_cs.active_mode = COLOR_BY_TIMESTAMP;
					draw_color_setup_timestamp_tab();
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("By wildcards")) {
					g_cs.active_mode = COLOR_BY_WPATTERN;
					draw_color_setup_wpattern_tab();
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}

			ImGui::Separator();
			// Port of csdialog_ok_button_cb(): commits the scratch
			// config and switches the live color mode to whichever tab
			// is currently selected (gtk_notebook_get_current_page()'s
			// exact equivalent here -- only the active BeginTabItem()
			// body runs each frame, so g_cs.active_mode always reflects
			// it). Stays open afterward (this is "Apply", not
			// OK+destroy) since this window is reused across opens
			// rather than re-created.
			//
			// The color_write_config() call has no counterpart in
			// dialog.c's actual OK handler -- that function only ever
			// called color_set_config()/window_set_color_mode(). Nvstore
			// persistence in the GTK build is dead code: callbacks.c's
			// "File -> Save settings" is the only caller of
			// color_write_config(), and it is `#if 0`'d out behind a
			// "Configuration file not yet implemented" message. This
			// port adds the write dialog.c's OK button always should
			// have made, per this task's explicit persistence
			// requirement.
			if (ImGui::Button("Apply")) {
				color_set_config(&g_cs.scratch, g_cs.active_mode);
				color_write_config();
			}
		}
		ImGui::End();
	}

	if (!g_cs.open && g_cs.have_scratch) {
		color_config_destroy(&g_cs.scratch);
		g_cs.have_scratch = false;
		g_new_pattern_text.clear();
	}
}

} // namespace

void
ui_dialogs_open_color_setup(void)
{
	// Port of dialog_color_setup()'s scratch-copy setup
	// (color_get_config()) -- but reused rather than re-created per
	// call (see ui_dialogs.h), so a *second* open first has to drop
	// whatever the previous open's scratch still owns (its
	// by_wpattern.wpgroup_list allocations), the same cleanup
	// csdialog_destroy_cb() ran on window destruction.
	if (g_cs.have_scratch)
		color_config_destroy(&g_cs.scratch);
	g_cs.scratch = ColorConfig{}; // by_wpattern.wpgroup_list must start NULL for color_config_copy() (inside color_get_config()) to build a fresh list rather than appending to garbage
	color_get_config(&g_cs.scratch);
	g_cs.have_scratch = true;
	g_new_pattern_text.clear(); // old groups' input buffers are gone with the old scratch

	// Port of dialog_color_setup()'s "Set page to current color mode":
	// GTK's gtk_notebook_set_current_page() vs. ImGui's tab bar, which
	// has no imperative "select this tab" call outside of
	// ImGuiTabItemFlags_SetSelected on the *next* BeginTabItem() --
	// deliberately not wired up (YAGNI: defaulting to the first tab,
	// like every other multi-tab window in this frontend, costs the
	// user one click and avoids a second, ImGui-version-fragile API).
	g_cs.active_mode = color_get_mode();
	cs_reset_time_sliders_from_scratch();
	g_cs.open = true;
}

namespace {

// ============================================================
// Properties
// ============================================================

struct PropContentsRow {
	std::string name;
	std::string size_text;
	bool is_dir;
};

struct PropertiesState {
	bool open = false;
	bool valid = false;
	NodeType type = NODE_UNKNOWN;
	std::string name, prefix, owner, group, atime, mtime, ctime;
	std::string size_text, size_abbr, alloc_text, alloc_abbr;
	std::string subtree_size, subtree_abbr;
	std::string file_type_desc;      // NODE_REGFILE only
	std::string target, abstarget;   // NODE_SYMLINK only
	void *target_node = nullptr;     // GNode*, resolved once at open time -- see ui_dialogs_close_properties()
	bool target_lookup_enabled = false;
	std::vector<PropContentsRow> contents; // NODE_DIRECTORY only
};

PropertiesState g_props;

} // namespace

void
ui_dialogs_close_properties(void)
{
	g_props.open = false;
	g_props.valid = false;
	g_props.target_node = nullptr;
	g_props.contents.clear();
}

void
ui_dialogs_open_properties(void *node_ptr)
{
	GNode *node = static_cast<GNode *>(node_ptr);
	if (node == nullptr)
		return;

	// get_node_info() (src/common.c) may run the 'file' command for a
	// regular file (get_file_type_desc(), only when HAVE_FILE_COMMAND)
	// and loop on its own gui_update() calls meanwhile. Both are safe
	// to run synchronously right here -- see ui_dialogs.h's doc
	// comment -- but NOT something to redo every frame, hence this
	// snapshot: get_node_info() reuses one static struct across calls,
	// so every field below is copied into owned storage immediately.
	const struct NodeInfo *info = get_node_info(node);

	g_props.type = NODE_DESC(node)->type;
	g_props.name = info->name;
	g_props.prefix = info->prefix;
	g_props.owner = info->user_name;
	g_props.group = info->group_name;
	g_props.atime = info->atime;
	g_props.mtime = info->mtime;
	g_props.ctime = info->ctime;

	g_props.contents.clear();
	g_props.target_node = nullptr;
	g_props.target_lookup_enabled = false;

	if (NODE_IS_DIR(node)) {
		g_props.subtree_size = info->subtree_size;
		g_props.subtree_abbr = info->subtree_size_abbr;
		// Port of dir_contents_list() (src/filelist.c): that function
		// is GTK-only (builds a GtkListStore) and not part of
		// libfsvcore, so this walks the live GNode children directly
		// instead -- the same reason src/sdl/ui_panels.cpp's file-list
		// panel already does this rather than calling filelist.c.
		for (GNode *c = node->children; c != nullptr; c = c->next) {
			PropContentsRow row;
			row.name = NODE_DESC(c)->name;
			row.is_dir = NODE_IS_DIR(c);
			const int64 sz = row.is_dir ?
			    DIR_NODE_DESC(c)->subtree.size : NODE_DESC(c)->size;
			row.size_text = abbrev_size(sz);
			g_props.contents.push_back(std::move(row));
		}
	} else {
		g_props.size_text = info->size;
		g_props.size_abbr = info->size_abbr;
		g_props.alloc_text = info->size_alloc;
		g_props.alloc_abbr = info->size_alloc_abbr;
	}

	if (g_props.type == NODE_REGFILE)
		g_props.file_type_desc = info->file_type_desc;

	if (g_props.type == NODE_SYMLINK) {
		g_props.target = info->target;
		g_props.abstarget = info->abstarget;

		// Port of look_at_target_node_cb()/dialog_node_properties()'s
		// symlink-target eligibility check: no button action at all if
		// the target isn't in the scanned tree, or -- in TreeV -- if
		// reaching it needs expanding a still-collapsed ancestor
		// (unbuilt TreeV geometry has no definite location to fly to).
		GNode *target_node = node_named(info->abstarget);
		if (target_node != nullptr && globals.fsv_mode == FSV_TREEV)
			if (NODE_IS_DIR(target_node->parent))
				if (!dirtree_entry_expanded(target_node->parent))
					target_node = nullptr;
		g_props.target_node = target_node;
		g_props.target_lookup_enabled = (target_node != nullptr);
	}

	g_props.valid = true;
	g_props.open = true;
}

namespace {

void
draw_properties_window(void)
{
	if (!g_props.open || !g_props.valid)
		return;

	ImGui::SetNextWindowSize(ImVec2(420, 380), ImGuiCond_FirstUseEver);
	// "###properties_window" pins the ImGui window ID regardless of the
	// visible title, so reopening Properties on a *different* node
	// reuses the same window (position/size survive) instead of ImGui
	// treating "Properties: foo.txt" and "Properties: bar.txt" as two
	// unrelated windows -- the "single reusable" option ui_dialogs.h
	// offers over dialog.c's one-new-GtkWindow-per-request original.
	const std::string title = "Properties: " + g_props.name + "###properties_window";
	if (ImGui::Begin(title.c_str(), &g_props.open)) {
		if (ImGui::BeginTabBar("properties_tabs")) {
			if (ImGui::BeginTabItem("General")) {
				ImGui::Text("Name: %s", g_props.name.c_str());
				ImGui::Text("Type: %s", node_type_label(g_props.type));
				ImGui::Text("Location: %s", g_props.prefix.c_str());
				ImGui::Separator();
				if (g_props.type == NODE_DIRECTORY)
					ImGui::Text("Total size: %s bytes (%s)",
					    g_props.subtree_size.c_str(), g_props.subtree_abbr.c_str());
				else {
					ImGui::Text("Size: %s bytes (%s)",
					    g_props.size_text.c_str(), g_props.size_abbr.c_str());
					ImGui::Text("Allocation: %s bytes (%s)",
					    g_props.alloc_text.c_str(), g_props.alloc_abbr.c_str());
				}
				ImGui::Separator();
				ImGui::Text("Owner: %s", g_props.owner.c_str());
				ImGui::Text("Group: %s", g_props.group.c_str());
				ImGui::Separator();
				ImGui::Text("Modified: %s", g_props.mtime.c_str());
				ImGui::Text("AttribCh: %s", g_props.ctime.c_str());
				ImGui::Text("Accessed: %s", g_props.atime.c_str());
				ImGui::EndTabItem();
			}
			if (g_props.type == NODE_DIRECTORY && ImGui::BeginTabItem("Contents")) {
				ImGui::Text("This directory contains %zu entries:", g_props.contents.size());
				if (ImGui::BeginTable("contents", 2,
				    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
				    ImVec2(0.0f, 220.0f))) {
					ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
					ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
					ImGui::TableHeadersRow();
					for (const auto &row : g_props.contents) {
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(row.is_dir ? "[DIR] " : "");
						ImGui::SameLine(0.0f, 0.0f);
						ImGui::TextUnformatted(row.name.c_str());
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(row.size_text.c_str());
					}
					ImGui::EndTable();
				}
				ImGui::Text("Total size: %s bytes (%s)",
				    g_props.subtree_size.c_str(), g_props.subtree_abbr.c_str());
				ImGui::EndTabItem();
			}
			if (g_props.type == NODE_REGFILE && ImGui::BeginTabItem("File type")) {
				ImGui::TextUnformatted("This file is recognized as:");
				ImGui::TextWrapped("%s", g_props.file_type_desc.c_str());
				ImGui::EndTabItem();
			}
			if (g_props.type == NODE_SYMLINK && ImGui::BeginTabItem("Target")) {
				ImGui::TextUnformatted("This symlink points to:");
				ImGui::TextWrapped("%s", g_props.target.c_str());
				ImGui::Spacing();
				ImGui::TextUnformatted("Absolute name of target:");
				if (g_props.target == g_props.abstarget)
					ImGui::TextWrapped("(same as above)");
				else
					ImGui::TextWrapped("%s", g_props.abstarget.c_str());
				ImGui::Spacing();
				ImGui::BeginDisabled(!g_props.target_lookup_enabled);
				if (ImGui::Button("Look at target node")) {
					GNode *target = static_cast<GNode *>(g_props.target_node);
					// Port of look_at_target_node_cb(): expand a
					// collapsed ancestor first so the camera has
					// somewhere real to fly to.
					if (NODE_IS_DIR(target->parent))
						if (!dirtree_entry_expanded(target->parent))
							colexp(target->parent, COLEXP_EXPAND_ANY);
					camera_look_at(target);
					g_props.open = false; // dialog.c's button also closes the window (close_cb wired to "clicked")
				}
				ImGui::EndDisabled();
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
		ImGui::Separator();
		if (ImGui::Button("Close"))
			g_props.open = false;
	}
	ImGui::End();

	if (!g_props.open)
		ui_dialogs_close_properties();
}

} // namespace

void
ui_dialogs_draw(void)
{
	draw_color_setup_window();
	draw_properties_window();
}

bool
ui_dialogs_handle_escape(void)
{
	// Order is arbitrary (the two windows are independent and neither
	// is modal) but fixed and documented rather than left implicit:
	// Properties first, then Color Setup. If both happen to be open, one
	// Escape closes Properties; a second closes Color Setup. Each branch
	// just flips the same `open` flag each window's own close affordance
	// already flips (draw_properties_window()'s "Close" button;
	// draw_color_setup_window()'s title-bar X, via `ImGui::Begin(...,
	// &g_cs.open)`) -- the next ui_dialogs_draw() call runs the exact
	// same teardown (ui_dialogs_close_properties() /
	// color_config_destroy()) either way, nothing bypassed.
	if (g_props.open) {
		g_props.open = false;
		return true;
	}
	if (g_cs.open) {
		g_cs.open = false;
		return true;
	}
	return false;
}

/* end ui_dialogs.cpp */
