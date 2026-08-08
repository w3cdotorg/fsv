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

#include <imgui.h>

#include "app.h"

extern "C" {
#include "common.h"
#include "camera.h"
#include "color.h"
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
