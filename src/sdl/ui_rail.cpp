// src/sdl/ui_rail.cpp — SPDX-License-Identifier: MIT
//
// fsn-mode Task A3: the left-hand camera control rail. See ui_rail.h's
// file header for the bigger picture; Task A2's bottom-center "ages:"
// legend bar (ui_legend_draw(), below) lives in this same file per the
// plan's Target File Structure.
//
// This wires camera control logic that has existed in src/camera.c since
// before this port (camera_look_at_previous(), camera_birdseye_view(),
// camera_scrollbar_moved()) but was never reachable from the SDL/ImGui
// UI — src/window.c's GTK toolbar is the only frontend that has ever
// called any of it. No new camera math: every button/slider below is a
// thin call into an entry point that already exists in camera.h.
//
// Tilt/Height axis mapping (per task-A3-brief.md: "label ours to match
// what the camera math actually does per mode, and say so in a comment
// if the mapping is imperfect" — verified by reading camera.c's
// mapv_scrollbar_move()/treev_scrollbar_move(), not guessed):
//
//   - MapV: axis 0 (X) moves MAPV_CAMERA target.x sideways across the
//     current directory's top face and yaws the camera (theta) to keep
//     facing it — a lateral pan with compensating turn. Axis 1 (Y) moves
//     target.y forward/back *and* pitches the camera (phi) as it does —
//     the felt effect is flying toward/away from the floor while looking
//     more steeply down as you get closer, which reads as changing your
//     viewing height over the map.
//   - TreeV: axis 0 (X) rotates TREEV_CAMERA target.theta around the
//     tree's central axis (yaw only, no lateral position change) — still
//     a turn, so still "Tilt"-flavored, but a spin around the tree rather
//     than a sideways pan. Axis 1 (Y) moves target.r, the radial distance
//     from the tree's axis — closer to a dolly than a height change.
//
// So "Tilt" (axis 0) and "Height" (axis 1) are upstream fsn's own labels
// for these two sliders (task-A2-brief.md's reference screenshot), kept
// here for continuity with the reference UI; the mapping to what each
// axis's math actually does is a reasonable-but-imperfect fit in MapV and
// a looser one in TreeV (rotation, not tilt; radius, not height). Flagged
// here rather than silently relabeled, per the brief.
#include "ui_rail.h"

#include <cstring>
#include <string>
#include <vector>

#include <SDL3/SDL.h> /* SDL_Log() -- "Go" click, same convention as ui_overview.cpp's click-to-look-at */
#include <imgui.h>

#include "app.h"

extern "C" {
#include "common.h"
#include "camera.h"
#include "color.h"
#include "colexp.h" /* colexp(), COLEXP_EXPAND_ANY -- see draw_mark_row()'s "Go" */
#include "dirtree.h" /* dirtree_entry_expanded() */
#include "nvstore.h"
#include "window.h"
}

#include "fsn-style.h" /* FsnAgeBucket, fsn_age_buckets[], FSN_AGE_BUCKET_COUNT */

// ---- Camera control rail --------------------------------------------------

static bool g_rail_visible = true;

bool
ui_rail_get_visible(void)
{
	return g_rail_visible;
}

void
ui_rail_set_visible(bool visible)
{
	g_rail_visible = visible;
}

// Buttons/sliders grey out under the same signal src/window.c's GTK
// toolbar already reacts to: window_set_access(FALSE)/(TRUE), which
// camera.c calls around every camera-owned pan (look-at, bird's-eye
// view) — see camera.c's camera_look_at_full()/camera_birdseye_view()/
// post_pan_end(). window_access_enabled() (src/sdl/stubs.c) is this
// frontend's read side of that same flag. No app_is_scanning() check
// here: ui_rail_draw()'s own early return below already guarantees a
// scan isn't running by the time this is called, so re-checking it here
// would be dead weight, not defense in depth.
static bool
rail_access_ok(void)
{
	return window_access_enabled();
}

// One vertical Tilt/Height slider. Reads the camera-pushed ScrollState
// for `axis` every frame (app_get_scroll_range(), fed by
// fsv_platform.set_scroll()) and, on a user drag, writes the new value
// back and forwards to camera_scrollbar_moved() via
// app_scrollbar_dragged() — the same two-step src/window.c's
// on_scrollbar_value_changed() performs for a real GtkAdjustment.
static void
draw_scroll_slider(int axis, const char *label, bool enabled)
{
	double lower, upper, page, value;
	app_get_scroll_range(axis, &lower, &upper, &page, &value);

	// GtkAdjustment convention (src/fsv-platform.h's doc comment on
	// set_scroll()): `value` ranges over [lower, upper-page], not
	// [lower, upper] — the latter would let the slider claim positions
	// where the page's far edge runs past `upper`. When the current mode
	// has no real scrollable range (DiscV's discv_get_scrollbar_state()
	// is a TODO stub; an unexpanded TreeV root falls back to
	// null_get_scrollbar_state()), lower == upper-page and the slider is
	// simply pinned — harmless, and `enabled` already greys it out in
	// those cases via the caller's mode check.
	float v = (float)value;
	const float v_min = (float)lower;
	const float v_max = (float)(upper - page);

	ImGui::PushID(axis);
	ImGui::BeginGroup();
	ImGui::TextUnformatted(label);
	ImGui::BeginDisabled(!enabled);
	if (ImGui::VSliderFloat("##slider", ImVec2(36.0f, 160.0f), &v, v_min,
	    v_max, ""))
		app_scrollbar_dragged(axis, (double)v);
	ImGui::EndDisabled();
	if (!enabled && ImGui::IsItemHovered())
		ImGui::SetTooltip("Only available in MapV/TreeV");
	ImGui::EndGroup();
	ImGui::PopID();
}

// ---- Marks (bookmarks) -----------------------------------------------
//
// fsn-mode Task C2. Upstream fsn's left-rail "Marks" list (task-C2-
// brief.md's reference screenshot): named bookmarks of nodes in the
// landscape, with "go to" and "delete" per row and a "Mark here" button
// that bookmarks globals.current_node. A slight extension of the
// original: shown in every mode, not FSN-only -- a mark is just a node
// bookmark, and there's nothing FSN-specific about wanting to jump back
// to a node from MapV or TreeV either.
//
// Persistence mirrors src/color.c's wpattern-group vector round trip
// exactly (nvs_vector_begin/nvs_path_present/nvs_vector_end around a
// repeated "mark" node, each holding scalar "name"/"path" children) --
// see color_read_config()/color_write_config() for the pattern this
// copies. Every mutation (add/delete/rename) rewrites the whole vector
// immediately: there is no explicit "Save" step to wire up, unlike the
// Color Setup dialog's Apply button, so a rewrite-on-every-change is the
// simplest thing that is still always correct.
//
// Stores each mark's node as an absolute path STRING
// (node_absname()'s raw-byte format), not a GNode pointer -- Task B1's
// UAF notes are exactly why: a rescan or Change Root frees and rebuilds
// the whole fstree, so a pointer captured before that would dangle.
// Resolution back to a live GNode* happens at draw time (to grey out a
// row whose path no longer exists) and at "go to" time, via
// src/common.c's node_from_absname() -- cheap enough to redo every
// frame for a handful of marks, and it sidesteps needing any dedicated
// "invalidate marks on rescan" hook.
namespace {

struct Mark {
	std::string name; // user label; defaults to the node's display name
	std::string path; // node_absname() raw bytes -- see node_from_absname()
};

std::vector<Mark> g_marks;

// Which row (by index into g_marks) is currently showing its inline
// rename InputText, or -1 if none. Index-based rather than keyed by
// path/pointer: simplest thing that works for "at most one row editing
// at a time", and it's reset to -1 on every mutation that could move
// indices around (delete) so it can never point at the wrong row.
int g_editing_index = -1;
char g_edit_buf[256];

const char key_marks[] = "marks";
const char key_marks_mark[] = "mark";
const char key_marks_name[] = "name";
const char key_marks_path[] = "path";

// Full rewrite of the "marks" vector -- same shape as
// color_write_config()'s ColorByWPattern section: change into the
// vector's own path, delete whatever was there before, write every
// entry inside a fresh nvs_vector_begin()/nvs_vector_end() pair.
void
marks_write_config(void)
{
	NVStore *fsvrc = nvs_open(CONFIG_FILE);

	nvs_change_path(fsvrc, key_marks);
	nvs_delete_recursive(fsvrc, ".");

	nvs_vector_begin(fsvrc);
	for (const Mark &m : g_marks) {
		nvs_change_path(fsvrc, key_marks_mark);
		nvs_write_string(fsvrc, key_marks_name, m.name.c_str());
		nvs_write_string(fsvrc, key_marks_path, m.path.c_str());
		nvs_change_path(fsvrc, "..");
	}
	nvs_vector_end(fsvrc);

	nvs_close(fsvrc);
}

// Loads the "marks" vector from ~/.fsvrc -- same shape as
// color_read_config()'s ColorByWPattern section, including the
// "nvs_vector_end() before leaving the vector's own path" step that
// Task 5.3's fix round found missing there (a stray-open vector would
// otherwise corrupt whatever nvstore key came right after this one).
// Called once at startup (ui_marks_init(), before any filesystem has
// been scanned), so this only ever populates strings -- no GNode
// resolution happens here.
void
marks_read_config(void)
{
	NVStore *fsvrc = nvs_open(CONFIG_FILE);

	g_marks.clear();
	nvs_change_path(fsvrc, key_marks);
	nvs_vector_begin(fsvrc);
	while (nvs_path_present(fsvrc, key_marks_mark)) {
		nvs_change_path(fsvrc, key_marks_mark);

		char *name = nvs_read_string_default(fsvrc, key_marks_name, "");
		char *path = nvs_read_string_default(fsvrc, key_marks_path, "");
		g_marks.push_back(Mark{ name, path });
		free(name); /* !xfree -- nvstore.c's xstrdup is plain strdup */
		free(path);

		nvs_change_path(fsvrc, "..");
	}
	nvs_vector_end(fsvrc);

	nvs_close(fsvrc);
}

// One "Marks" row: label (double-click to rename inline) + Go + Delete.
// Returns true if the caller should erase this entry afterward (the
// Delete button was clicked) -- deletion is deferred to the caller so
// this function never mutates the vector it's being called *from* while
// iterating it.
bool
draw_mark_row(int index, Mark &m, bool access_ok)
{
	bool delete_requested = false;

	ImGui::PushID(index);

	// Resolved fresh every frame -- see this section's file-header
	// comment on why redoing this walk beats caching a pointer.
	GNode *target = node_from_absname(m.path.c_str());
	const bool resolved = (target != nullptr);

	if (index == g_editing_index) {
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
		ImGui::InputText("##rename", g_edit_buf, sizeof(g_edit_buf),
		    ImGuiInputTextFlags_EnterReturnsTrue |
		    ImGuiInputTextFlags_AutoSelectAll);
		if (ImGui::IsItemDeactivated()) {
			// Commits on Enter or on losing focus either way (both
			// deactivate the item in the same frame); an empty edit
			// (the field cleared to nothing) is discarded rather than
			// leaving the mark unnamed. Does not trim/reject a
			// whitespace-only edit -- that's a real (if odd) label a
			// user could deliberately type, not worth guarding against.
			if (g_edit_buf[0] != '\0')
				m.name = g_edit_buf;
			g_editing_index = -1;
			marks_write_config();
		}
	} else {
		if (resolved)
			ImGui::TextUnformatted(m.name.c_str());
		else
			ImGui::TextDisabled("%s", m.name.c_str());
		if (ImGui::IsItemHovered()) {
			// Display form for a live node (UTF-8-safe, NFC-composed --
			// same node_absname_display() distinction Task B-era code
			// already draws for the status bar/context menu); the raw
			// stored path for a missing one, since there's no live node
			// left to ask for a display form of it.
			if (resolved)
				ImGui::SetTooltip("%s", node_absname_display(target));
			else
				ImGui::SetTooltip("Not found here: %s", m.path.c_str());
		}
		if (ImGui::IsItemHovered() &&
		    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
			g_editing_index = index;
			strncpy(g_edit_buf, m.name.c_str(), sizeof(g_edit_buf) - 1);
			g_edit_buf[sizeof(g_edit_buf) - 1] = '\0';
		}
	}

	ImGui::SameLine();
	ImGui::BeginDisabled(!resolved || !access_ok);
	if (ImGui::SmallButton("Go")) {
		// Port of ui_dialogs.cpp's "Look at target node" button: a
		// resolved node can still sit under a collapsed directory (a
		// mark made before a Change Root, or one nobody has expanded
		// since), and camera_look_at_full() asserts that its immediate
		// parent is already expanded -- discovered the hard way, by
		// this exact assertion firing during this task's own headed
		// verification, when going to a mark under a still-collapsed
		// ancestor. COLEXP_EXPAND_ANY (colexp.c) walks the *whole*
		// ancestor chain, not just the immediate parent, so this covers
		// a mark buried several directories deep, not only one level.
		if (NODE_IS_DIR(target->parent) &&
		    !dirtree_entry_expanded(target->parent))
			colexp(target->parent, COLEXP_EXPAND_ANY);
		SDL_Log("marks: go to %s", node_absname_display(target));
		camera_look_at(target);
	}
	ImGui::EndDisabled();

	ImGui::SameLine();
	if (ImGui::SmallButton("X"))
		delete_requested = true;

	ImGui::PopID();

	return delete_requested;
}

// The "Marks" section itself: "Mark here" button, then one row per
// bookmark. Joins the camera rail below its Tilt/Height sliders, per
// the plan's Target File Structure -- called from ui_rail_draw() below,
// not exposed separately (there is exactly one caller, same as
// draw_scroll_slider() above).
void
draw_marks_section(bool access_ok, float btn_w)
{
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	ImGui::TextUnformatted("Marks");

	// Gated on having a current node, not on access_ok: bookmarking
	// doesn't touch the camera, so there's no in-flight-morph reason to
	// block it the way Reset/Go back/Front view are blocked above.
	ImGui::BeginDisabled(globals.current_node == nullptr);
	if (ImGui::Button("Mark here", ImVec2(btn_w, 0.0f))) {
		// Task 4 (ux-polish-batch) de-dup: compare against the same
		// node_absname() form each row's Mark::path already stores (see
		// the struct's comment above and the add path just below) --
		// not a freshly-derived variant that could drift from it.
		// Marking a node that's already bookmarked used to push a
		// second, identical row; now it just focuses the existing row's
		// inline rename field, mirroring draw_mark_row()'s own
		// double-click-to-rename entry point, so the click still gives
		// visible feedback.
		const char *absname = node_absname(globals.current_node);
		int existing = -1;
		for (int i = 0; i < (int)g_marks.size(); i++) {
			if (g_marks[i].path == absname) {
				existing = i;
				break;
			}
		}
		if (existing >= 0) {
			g_editing_index = existing;
			strncpy(g_edit_buf, g_marks[existing].name.c_str(),
			    sizeof(g_edit_buf) - 1);
			g_edit_buf[sizeof(g_edit_buf) - 1] = '\0';
		} else {
			Mark m;
			m.path = absname;
			const char *dname = NODE_DNAME(globals.current_node);
			m.name = (strlen(dname) > 0) ? dname : _("/. (root)");
			g_marks.push_back(std::move(m));
			marks_write_config();
		}
	}
	ImGui::EndDisabled();

	int mark_to_delete = -1;
	for (int i = 0; i < (int)g_marks.size(); i++)
		if (draw_mark_row(i, g_marks[i], access_ok))
			mark_to_delete = i;

	if (mark_to_delete >= 0) {
		g_marks.erase(g_marks.begin() + mark_to_delete);
		g_editing_index = -1; // indices just shifted; don't point at the wrong row
		marks_write_config();
	}
}

} // namespace

void
ui_marks_init(void)
{
	marks_read_config();
}

void
ui_rail_draw(void)
{
	if (!g_rail_visible)
		return;
	// Hidden outright during a scan or before the first filesystem has
	// loaded — matches ui_panels_draw()'s Directory Tree panel (same
	// reasoning: no stable current_node/root_dnode for any of these
	// buttons to point at yet).
	if (app_is_scanning() || globals.fsv_mode == FSV_NONE)
		return;
	if (globals.fstree == nullptr || root_dnode == nullptr)
		return;

	const ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(
	    ImVec2(vp->WorkPos.x + 8.0f, vp->WorkPos.y + 8.0f),
	    ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(150.0f, 380.0f), ImGuiCond_FirstUseEver);

	if (!ImGui::Begin("Camera Rail", &g_rail_visible)) {
		ImGui::End();
		return;
	}

	const bool access_ok = rail_access_ok();
	const FsvMode mode = globals.fsv_mode;
	const float btn_w = ImGui::GetContentRegionAvail().x;

	ImGui::BeginDisabled(!access_ok);

	// Re-runs the current mode's mode-entry sequence in place
	// (geometry_init()/camera_init()/intro pan) — see app.h's
	// app_reset_camera() doc comment for why this needs its own entry
	// point rather than reusing app_switch_mode() directly.
	if (ImGui::Button("Reset", ImVec2(btn_w, 0.0f)))
		app_reset_camera();

	if (ImGui::Button("Go back", ImVec2(btn_w, 0.0f)))
		camera_look_at_previous();

	// Toggle button: highlighted while bird's-eye view is active.
	// window_birdseye_active() is this frontend's answer to GTK's
	// private birdseye_view_tbutton_w, kept in sync by
	// window_birdseye_view_off() whenever the *core* exits bird's-eye
	// view on its own (e.g. camera_look_at_full() picking a new node
	// while airborne) — not just by this button's own clicks.
	{
		const bool birdseye_on = window_birdseye_active();
		if (birdseye_on)
			ImGui::PushStyleColor(ImGuiCol_Button,
			    ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
		const bool clicked = ImGui::Button("Birds eye", ImVec2(btn_w, 0.0f));
		if (birdseye_on)
			ImGui::PopStyleColor();
		if (clicked) {
			const boolean going_up = birdseye_on ? FALSE : TRUE;
			camera_birdseye_view(going_up);
			window_birdseye_set_active(going_up ? TRUE : FALSE);
		}
	}

	// "Front view": camera.h has no pose-specific "front view" entry
	// point (upstream fsn's actual front-view pose isn't reproduced by
	// this port — no new camera math per the plan's constraints), so
	// this is the brief's own closest existing analog: re-run
	// camera_look_at() on the current node, returning to *a* canonical,
	// non-manual framing of it. An approximation, not a port.
	if (ImGui::Button("Front view", ImVec2(btn_w, 0.0f)))
		camera_look_at(globals.current_node);

	ImGui::EndDisabled();

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// Sliders only mean anything in the two modes whose camera actually
	// consumes scrollbar-driven input: mapv_scrollbar_move()/
	// treev_scrollbar_move() in camera.c. DiscV's discv_scrollbar_move()
	// is explicitly marked "?????" (dead-reckoning at best) and its
	// discv_get_scrollbar_state() is a TODO stub that just echoes back
	// whatever scroll_state[] already held, so it never has a real range
	// to display in the first place.
	const bool sliders_ok = access_ok &&
	    (mode == FSV_MAPV || mode == FSV_TREEV);

	draw_scroll_slider(0, "Tilt", sliders_ok);
	ImGui::SameLine();
	draw_scroll_slider(1, "Height", sliders_ok);

	if (access_ok && !sliders_ok)
		ImGui::TextDisabled("MapV/TreeV only");

	draw_marks_section(access_ok, btn_w);

	ImGui::End();
}

// ---- Ages legend -----------------------------------------------------

void
ui_legend_draw(void)
{
	// Only meaningful for the one color mode/spectrum combination this
	// legend describes -- see color.c's time_color( ): every other
	// combination colors nodes by node type, wildcard pattern, or a
	// continuous rainbow/heat/gradient spectrum that this bucket
	// legend says nothing about.
	if (color_get_mode() != COLOR_BY_TIMESTAMP)
		return;
	if (color_timestamp_spectrum_type() != SPECTRUM_FSN_BUCKETS)
		return;

	// Bottom-center of the viewport, anchored by its own bottom-center
	// pivot (ImVec2(0.5f, 1.0f)) so the auto-resized window grows
	// upward/outward from that fixed point instead of drifting as its
	// content width changes -- same ImGuiViewport-anchored idiom
	// main.cpp's own scan overlay uses (SetNextWindowPos off
	// GetMainViewport()'s WorkPos/WorkSize), just anchored to a
	// different corner.
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(
	    ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
	           vp->WorkPos.y + vp->WorkSize.y - 12.0f),
	    ImGuiCond_Always, ImVec2(0.5f, 1.0f));
	ImGui::SetNextWindowBgAlpha(0.85f);
	if (ImGui::Begin("##ages_legend", nullptr,
	    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
	    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
	    ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs)) {
		ImGui::TextUnformatted("ages:");
		for (int i = 0; i < FSN_AGE_BUCKET_COUNT; i++) {
			const FsnAgeBucket &b = fsn_age_buckets[i];
			ImGui::SameLine();
			// A colored, non-interactive-looking chip per bucket:
			// SmallButton for its tight padding and rectangular
			// shape, with the window's own NoInputs flag (above)
			// already making it inert to hover/click -- this is a
			// swatch+label, not a real button.
			ImGui::PushID(i);
			ImGui::PushStyleColor(ImGuiCol_Button,
			    ImVec4(b.rgb[0], b.rgb[1], b.rgb[2], 1.0f));
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
			ImGui::SmallButton(b.label);
			ImGui::PopStyleColor(2);
			ImGui::PopID();
		}
	}
	ImGui::End();
}

/* end ui_rail.cpp */
