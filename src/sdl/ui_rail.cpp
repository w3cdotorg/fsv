// src/sdl/ui_rail.cpp — SPDX-License-Identifier: MIT
//
// fsn-mode Task A2: the bottom-center "ages:" legend bar. See
// ui_rail.h's file header for the bigger picture (Task A3 adds the
// camera control rail to this same file).
#include "ui_rail.h"

#include <imgui.h>

extern "C" {
#include "common.h"
#include "color.h"
}

#include "fsn-style.h" /* FsnAgeBucket, fsn_age_buckets[], FSN_AGE_BUCKET_COUNT */

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
