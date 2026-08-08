// src/sdl/ui_overview.cpp — SPDX-License-Identifier: MIT
//
// fsn-mode Task C1: the overview window. See ui_overview.h for the split
// between the two entry points; this file holds the ImGui window, the
// click-to-look-at mapping, and the "has anything moved?" test that
// decides when the mini-map is worth re-rendering.
//
// The renderer side -- the top-down orthographic pass, the camera
// marker, the texture itself -- is src/sdl/gpu.cpp's
// gpu_overview_render(), next to the three offscreen paths it is modeled
// on (gpu_pick(), --screenshot, --record).
//
// TEXTURE BINDING. ImGui 1.92's SDL_GPU backend takes a raw
// SDL_GPUTexture* as its ImTextureID and supplies its own sampler
// (imgui_impl_sdlgpu3.cpp: `texture_sampler_binding.texture =
// (SDL_GPUTexture*)(intptr_t)pcmd->GetTexID()`, sampler =
// bd->CurrentSampler) -- verified by reading the vendored backend, not
// assumed: before 2025/08/08 the same backend wanted a pointer to an
// SDL_GPUTextureSamplerBinding instead, and passing one to the other
// crashes. ImGui::Image() takes an ImTextureRef, which has an implicit
// constructor from ImTextureID, so the cast is the whole story. The
// texture is created with SDL_GPU_TEXTUREUSAGE_SAMPLER for this.
#include "ui_overview.h"

#include <cfloat>

#include <imgui.h>

#include "app.h"
#include "gpu_internal.hpp"

extern "C" {
#include "common.h"
#include "animation.h" /* redraw( ) */
#include "camera.h"
#include "geometry-fsn.h"
#include "window.h" /* window_access_enabled( ) */
}

#include "fsn-style.h" /* FSN_OVERVIEW_WIDTH/HEIGHT */

// Shown by default in FSN mode, per the plan. Not persisted: neither is
// the camera rail's, and nvstore carries only the color/landscape
// preferences today.
static bool g_visible = true;

bool
ui_overview_get_visible(void)
{
	return g_visible;
}

void
ui_overview_set_visible(bool visible)
{
	g_visible = visible;
}

// ---- Re-render policy -----------------------------------------------
//
// Everything the mini-map's pixels depend on, in one comparable value.
// The mini-map is re-rendered exactly when this changes, so an idle
// application re-renders it zero times however many frames it draws.
//
// The camera contributes its whole pose, not just its ground position:
// the marker's heading comes from theta, and its ground position is
// derived from target + distance and phi (see gpu.cpp's
// draw_overview_marker()), so all six numbers move the marker.
//
// `deployment_sum` is the awkward one, and the reason this is a walk
// rather than six scalars. Expanding or collapsing a directory changes
// which pedestals are drawn and, mid-morph, how tall their children
// stand -- with no camera movement at all if it was done from the
// context menu. There is no core-side "an animation is running" query to
// ask (src/animation.h has morph_* but no predicate), and adding one for
// this would be a core API change for a frontend cache. Summing the
// deployments is O(directories) on frames that are being drawn anyway,
// next to the two full tree walks geometry_draw() already performs, and
// it captures every intermediate value of the morph rather than just its
// endpoints.
namespace {

struct OverviewKey {
	int mode;
	unsigned int generation;
	double target_x, target_y, target_z;
	double theta, phi, distance;
	double deployment_sum;
};

double
deployment_sum(GNode *dnode)
{
	double sum = DIR_NODE_DESC(dnode)->deployment;
	GNode *node;

	// Same short-circuit as the draw pass: a collapsed directory's
	// children are not drawn, so their deployment cannot change what
	// the mini-map shows.
	if (DIR_COLLAPSED(dnode))
		return sum;

	for (node = dnode->children; node != nullptr; node = node->next)
		if (NODE_IS_DIR(node))
			sum += deployment_sum(node);

	return sum;
}

OverviewKey
current_key(void)
{
	OverviewKey key = {};
	GNode *root = fsn_layout_root();

	key.mode = (int)globals.fsv_mode;
	key.generation = fsn_layout_generation();
	// FSN reuses MapV's camera storage -- see camera.c's FSN_CAMERA_*
	// note and setup_modelview_matrix()'s shared FSV_FSN/FSV_MAPV case.
	key.target_x = MAPV_CAMERA(camera)->target.x;
	key.target_y = MAPV_CAMERA(camera)->target.y;
	key.target_z = MAPV_CAMERA(camera)->target.z;
	key.theta = camera->theta;
	key.phi = camera->phi;
	key.distance = camera->distance;
	key.deployment_sum = (root != nullptr) ? deployment_sum(root) : 0.0;

	return key;
}

bool
key_equal(const OverviewKey &a, const OverviewKey &b)
{
	// Exact comparison is the right test here: these are verbatim
	// copies of the same doubles, not the result of any arithmetic, so
	// "unchanged" really does mean bit-identical. An epsilon would only
	// add a threshold below which the marker silently stopped tracking
	// a slow camera.
	return a.mode == b.mode && a.generation == b.generation &&
	    a.target_x == b.target_x && a.target_y == b.target_y &&
	    a.target_z == b.target_z && a.theta == b.theta &&
	    a.phi == b.phi && a.distance == b.distance &&
	    a.deployment_sum == b.deployment_sum;
}

OverviewKey g_last_key;
bool g_have_key;

// True while the overview has something to show at all -- the same three
// conditions ui_rail_draw() gates on, plus FSN mode, plus the View menu
// toggle. Both entry points below check it, so the window and the render
// can never disagree about whether the overview is live.
bool
overview_live(void)
{
	if (!g_visible)
		return false;
	if (globals.fsv_mode != FSV_FSN)
		return false;
	if (app_is_scanning())
		return false;
	if (globals.fstree == nullptr || root_dnode == nullptr)
		return false;
	return true;
}

} // namespace

void
ui_overview_render(void)
{
	if (!overview_live())
		return;

	const bool was_valid = gpu_overview_texture() != nullptr;
	const OverviewKey key = current_key();

	if (was_valid && g_have_key && key_equal(key, g_last_key))
		return;

	if (!gpu_overview_render())
		return; // no layout yet, or not in a frame -- keep the last map

	g_last_key = key;
	g_have_key = true;

	if (!was_valid) {
		// The window drew a placeholder this frame (there was no
		// texture yet when ui_overview_draw() ran, several steps
		// earlier). Ask for one more frame so the map it is now
		// holding actually reaches the screen; without this the
		// placeholder would sit there until some unrelated event
		// happened to wake the main loop.
		redraw();
	}
}

// Turns a click at (u, v) in the image -- u/v in [0,1], v measured from
// the TOP edge, as ImGui reports item-relative positions -- into a flight
// to the nearest pedestal.
//
// The vertical flip is the only subtle part: the render's orthographic
// projection has world +y as NDC +y, and NDC +y is the *top* row of the
// texture (this renderer's clip-space convention, the same one
// gpu_pick() relies on to read back a top-left-origin pixel without a
// flip). So v == 0 is world max_y.
static void
overview_click(float u, float v)
{
	double x0, x1, y0, y1;
	GNode *node;

	gpu_overview_frame_rect(&x0, &x1, &y0, &y1);
	if (!(x1 > x0) || !(y1 > y0))
		return; // no successful render yet

	const double wx = x0 + (double)u * (x1 - x0);
	const double wy = y1 - (double)v * (y1 - y0);

	node = fsn_layout_nearest(wx, wy);
	if (node == nullptr)
		return;

	SDL_Log("overview: click (%.1f, %.1f) -> %s", wx, wy,
	    NODE_DESC(node)->name);
	camera_look_at(node);
}

void
ui_overview_draw(void)
{
	if (!overview_live())
		return;

	// Default placement: top-right of the viewport's work area, which is
	// where upstream fsn's own overview window sits in the reference
	// screenshot. FirstUseEver only -- once the user has moved or
	// resized it, imgui.ini owns the geometry.
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	const float default_w = 320.0f, default_h = 200.0f;
	ImGui::SetNextWindowPos(
	    ImVec2(vp->WorkPos.x + vp->WorkSize.x - default_w - 8.0f,
		   vp->WorkPos.y + 8.0f),
	    ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(default_w, default_h),
	    ImGuiCond_FirstUseEver);
	// Small enough to tuck away, large enough that the map is still a
	// map. No maximum: a user who wants the overview full-screen is
	// welcome to it (the texture is fixed-resolution, so it simply
	// scales up).
	ImGui::SetNextWindowSizeConstraints(ImVec2(140.0f, 110.0f),
	    ImVec2(FLT_MAX, FLT_MAX));

	// No scrollbars: the image is always fitted to the content region,
	// so there is never anything to scroll, and a scrollbar appearing
	// for one frame during a resize would shrink the region and fight
	// with the fit.
	if (!ImGui::Begin("Overview", &g_visible,
	    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
		ImGui::End();
		return;
	}

	SDL_GPUTexture *texture = gpu_overview_texture();
	if (texture == nullptr) {
		// First frame with the window open: ui_overview_render() has
		// not run yet this frame (it runs later, inside
		// submit_frame()), so there is nothing to show. It will
		// request another frame the moment there is.
		ImGui::TextDisabled("Rendering...");
		ImGui::End();
		return;
	}

	// Fit the fixed-aspect texture into whatever the window is now,
	// letterboxing rather than stretching -- a squashed map misreports
	// the shape of the landscape.
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	const float aspect = (float)FSN_OVERVIEW_WIDTH /
	    (float)FSN_OVERVIEW_HEIGHT;
	ImVec2 size = avail;
	if (avail.x / aspect <= avail.y)
		size.y = avail.x / aspect;
	else
		size.x = avail.y * aspect;
	if (size.x < 1.0f || size.y < 1.0f) {
		ImGui::End();
		return; // window collapsed to nothing this frame
	}

	// See this file's header for why a raw SDL_GPUTexture* is the
	// correct ImTextureID for this backend version.
	ImGui::Image((ImTextureID)(intptr_t)texture, size);

	// Click-to-look-at. Gated on window_access_enabled() for the same
	// reason src/sdl/ui_rail.cpp's buttons are: while the camera is
	// running a pan of its own, camera.c has told the frontend to keep
	// its hands off (window_set_access(FALSE)).
	if (ImGui::IsItemClicked() && window_access_enabled()) {
		const ImVec2 origin = ImGui::GetItemRectMin();
		const ImVec2 rect = ImGui::GetItemRectSize();
		const ImVec2 mouse = ImGui::GetIO().MousePos;

		if (rect.x > 0.0f && rect.y > 0.0f)
			overview_click((mouse.x - origin.x) / rect.x,
			    (mouse.y - origin.y) / rect.y);
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Click to fly to the nearest directory");

	ImGui::End();
}

/* end ui_overview.cpp */
