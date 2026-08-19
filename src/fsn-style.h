/* src/fsn-style.h — SPDX-License-Identifier: MIT
 *
 * Named constants for fsn-mode's visual style. Task A1 defines the
 * landscape (sky/ground) presets; later tasks add bucket/wire/spotlight
 * parameters here too (see .superpowers/sdd/2026-08-08-fsn-mode/'s plan,
 * "Target File Structure").
 *
 * Deliberately header-only: every including translation unit gets its
 * own internal-linkage copy of the small table below (`static const`,
 * not `extern`), which needs no defining .c file and so no meson.build
 * change in either frontend's source list. Plain C99, no GLib/SDL/ImGui
 * dependency of any kind — this is included by both C translation units
 * that are part of libfsvcore (src/color.c) and C++ ones that are not
 * (src/sdl/gpu.cpp, src/sdl/ui_main.cpp), exactly like src/gpu.h.
 */

#ifndef FSV_FSN_STYLE_H
#define FSV_FSN_STYLE_H

/* One landscape preset: a top-to-horizon sky gradient plus a flat,
 * unlit ground color. Values are linear 0..1 floats, fed straight into
 * gpu_set_color()'s r/g/b arguments (see src/sdl/gpu.cpp's
 * draw_landscape()) -- no gamma handling here or anywhere else in this
 * renderer (scene.frag has none either). */
typedef struct {
	const char *name;     /* nvstore token (src/color.c) + menu label
	                        * (src/sdl/ui_main.cpp) -- keep in sync with
	                        * color.c's tokens_landscape[] array */
	float sky_top[3];     /* color at the top of the visible viewport */
	float sky_horizon[3]; /* color where sky meets ground */
	float ground[3];      /* flat ground color */
} FsnLandscape;

#define FSN_LANDSCAPE_COUNT 3

/* Index passed to gpu_set_landscape() to draw nothing (today's plain
 * flat clear from Task 2.2, no sky/ground quads at all) -- distinct from
 * the "slate" preset below, which reproduces the same *look* by actually
 * drawing sky+ground quads in that color, exercising the same code path
 * the other two presets do. Nothing persists FSN_LANDSCAPE_OFF today
 * (src/color.c's default is "slate", index 2); it exists for
 * gpu_set_landscape()'s documented contract and any future caller that
 * wants the pre-A1 behavior verbatim. */
#define FSN_LANDSCAPE_OFF (-1)

/* Index of the "classic" preset in fsn_landscapes[] -- src/sdl/main.cpp's
 * FSN auto-landscape default (Task B3: entering FSN mode selects this
 * unless the user has explicitly chosen a landscape from the Display
 * menu, src/color.c's landscape_explicit()/landscape_set()). Named rather
 * than spelled 0 at the call site because "classic" being array index 0
 * is otherwise only implicit in fsn_landscapes[]'s declaration order and
 * color.c's parallel tokens_landscape[] -- see that array's own comment. */
#define FSN_LANDSCAPE_CLASSIC 0

/* Index of the "night" preset. Named for the same reason as
 * FSN_LANDSCAPE_CLASSIC above; its one consumer is the overview mini-map
 * (see FSN_OVERVIEW_* below), which the plan pins to the night palette
 * regardless of the main view's own landscape choice. */
#define FSN_LANDSCAPE_NIGHT 1

/* Colors eyeballed from the two reference screenshots (task-A1-brief.md's
 * "Spec sources"): 3060c037-069f-4715-a01e-c30e53e505a2.jpg (overview +
 * "fsn" window, oblique view, wires) and 35037135976_0d90f4a3d5_z.jpg
 * (inside-a-directory, spotlight, near-level camera). Sampling a clean
 * (geometry-free) vertical strip of each in Python/PIL --
 * x=480,y=252..322 in the first image, x=180,y=170..238 in the second --
 * both read a vivid, fully-saturated sky blue (~RGB 2,138,211) at the
 * top of the visible viewport, fading smoothly to a pale cyan-white
 * (~RGB 140,250,248) right at the horizon, over a medium green ground
 * that holds steady around ~RGB 65,140,90 near the horizon (it darkens
 * with on-screen distance from the horizon in both references, almost
 * certainly SGI IRIS GL's depth-cueing/fog on the ground polygon -- out
 * of scope for a flat gpu_set_color() ground plane, YAGNI for Task A1).
 * This is *not* the near-black navy top the plan's own first draft
 * guessed: the visible sky band in both screenshots is high enough
 * above the horizon (camera pitched down, showing mostly ground) that
 * it never reaches anywhere near the zenith, so a materially brighter
 * blue than "navy" is what the actual pixels show. */
static const FsnLandscape fsn_landscapes[FSN_LANDSCAPE_COUNT] = {
	{
		"classic",
		{ 0.008f, 0.541f, 0.827f }, /* sky_top: vivid IRIS-blue, ~(2,138,211) */
		{ 0.549f, 0.980f, 0.973f }, /* sky_horizon: pale cyan-white, ~(140,250,248) */
		{ 0.255f, 0.549f, 0.353f }, /* ground: medium green, ~(65,140,90) */
	},
	{
		/* Neither reference screenshot is a night scene -- both are
		 * plain daylight captures -- so unlike "classic" this has
		 * nothing to sample a pixel from. Calibrated by screenshot
		 * iteration (2026-08, plan 2026-08-14-theta-unwrap-night-
		 * preset.md) against the legibility constraints laid out there
		 * instead: a night sky with a readable horizon, the
		 * wires/labels/age-spectrum colors staying legible, and the
		 * selection spotlight's beam staying visible against the sky.
		 * Validated against two capture framings: the default FSN
		 * establishing shot (where the ground occludes all but the top
		 * ~14% of the sky_top->sky_horizon gradient -- draw_landscape( ),
		 * src/sdl/gpu.cpp) and one throwaway tilted capture (camera->phi
		 * forced to ~2 degrees, near-level, exposing ~46% of the
		 * gradient) confirming the fuller range still reads as night --
		 * dark, smoothly graduated, no washed-out band -- not just the
		 * thin strip the default framing shows. Even that tilted capture
		 * does not reach sky_horizon's own endpoint value: camera.c
		 * clamps camera->phi to [1, 90] (`CLAMP(camera->phi, 1.0, 90.0)`),
		 * so near-straight-down is as level as the camera ever gets, and
		 * at most roughly half the sky_top->sky_horizon gradient is ever
		 * actually on screen -- the ~46% the tilted capture observed is
		 * close to that ceiling, not a conservative sample of a much
		 * larger visible range. sky_horizon itself is therefore
		 * extrapolated from the visible ~14%/~46% bands, not directly
		 * observed at the horizon itself. The ground is
		 * deliberately unchanged -- see its own comment below. */
		"night",
		{ 0.012f, 0.02f, 0.09f }, /* sky_top: deep blue zenith, not pure black */
		{ 0.22f, 0.28f, 0.48f }, /* sky_horizon: moonlit glow */
		{ 0.255f, 0.549f, 0.353f }, /* ground: same green as classic -- the
		                             * ground doesn't relight at night any
		                             * more than the overview window's own
		                             * green background does (reference
		                             * screenshot 1's inset thumbnail).
		                             * Deliberately NOT touched by this
		                             * calibration pass: the overview
		                             * mini-map (FSN_OVERVIEW_* below) is
		                             * pinned to the night preset
		                             * regardless of the main view's own
		                             * landscape choice, and that inset's
		                             * background IS this ground value --
		                             * the pin makes it load-bearing for
		                             * the overview, not merely a color
		                             * this preset happens to share with
		                             * "classic". */
	},
	{
		/* Reproduces src/sdl/gpu.cpp's pre-A1 flat clear color exactly
		 * (Task 2.2's {0.08, 0.10, 0.12}), so switching to "slate" is a
		 * no-op regression check on the new sky/ground code path, not a
		 * new look -- and gives the port's current appearance a name so
		 * it can be a first-class menu choice instead of just "off". */
		"slate",
		{ 0.08f, 0.10f, 0.12f },
		{ 0.08f, 0.10f, 0.12f },
		{ 0.08f, 0.10f, 0.12f },
	},
};

/* ---- Overview mini-map (fsn-mode Task C1) --------------------------
 *
 * The picture-in-picture window in the top-right of reference screenshot
 * 3060c037-069f-4715-a01e-c30e53e505a2.jpg: the whole landscape seen
 * from straight above on a flat green field, wires fanning out from the
 * root, and a small dark marker where the camera is standing.
 *
 * The mini-map is rendered into a fixed-size offscreen texture (src/sdl/
 * gpu.cpp's gpu_overview_render( )) which ImGui then scales to whatever
 * size the user has dragged the window to. Fixed rather than
 * window-sized deliberately: a resolution that tracked the window would
 * have to destroy and recreate the texture (and its depth buffer) on
 * every drag frame, and 512x320 is already more pixels than the window's
 * default size shows. 16:10, so the default window is close to
 * pixel-for-pixel. */
#define FSN_OVERVIEW_WIDTH  512
#define FSN_OVERVIEW_HEIGHT 320

/* Blank margin around the landscape's own bounding box, as a fraction of
 * the larger ground dimension, so pedestals at the very edge are not
 * clipped by the viewport border. Eyeballed against the reference
 * screenshot's inset, whose landscape sits well clear of its frame. */
#define FSN_OVERVIEW_MARGIN 0.08

/* The camera marker: an isoceles triangle pointing the way the camera is
 * looking, centered on the camera's ground position. Its size is a
 * fraction of the framed area's half-width rather than a world-unit
 * constant, so it stays the same size on screen whatever the landscape's
 * scale -- a fixed world size would be a speck over a big tree and would
 * swamp a small one.
 *
 * COLOR. The reference's marker is a small black X on the green field.
 * This port draws a bright yellow arrow instead, for two reasons: it has
 * to read against the pedestals themselves (light grey in the reference,
 * but any color at all here, since this port colors nodes by type or
 * timestamp), and unlike an X an arrow also shows the heading, which is
 * the half of the camera's state a click-to-look-at user most wants back.
 * Marked as a deliberate departure rather than an eyeballed value. */
#define FSN_OVERVIEW_MARKER_FRAC 0.055
#define FSN_OVERVIEW_MARKER_R 1.00f
#define FSN_OVERVIEW_MARKER_G 0.90f
#define FSN_OVERVIEW_MARKER_B 0.10f

/* Vertical headroom left above the tallest object when placing the
 * top-down camera, in world units. Only has to be positive (an
 * orthographic projection has no perspective to gain or lose by
 * distance); it exists so the near clip plane sits strictly above the
 * geometry rather than exactly on it. */
#define FSN_OVERVIEW_HEADROOM 64.0


/* One entry in fsn's 7-bucket "ages:" legend (task-A2-brief.md's
 * reference screenshot, 35037135976_0d90f4a3d5_z.jpg -- a bottom status
 * bar reading "ages: 1 wk 2 wk 1 mo 3 mo 6 mo 1 yr > 1 yr", each label
 * on its own colored swatch). Consumed two ways: src/color.c's
 * SPECTRUM_FSN_BUCKETS coloring path steps a file's age (now minus its
 * chosen timestamp) through max_age_s to pick a color; src/sdl/
 * ui_rail.cpp's ui_legend_draw( ) just iterates the whole table to draw
 * the swatch+label bar verbatim. Same header-only shape as
 * FsnLandscape/fsn_landscapes[] above, for the same reason (shared by a
 * C translation unit -- color.c -- and a C++ one -- ui_rail.cpp -- with
 * no meson.build change needed for either). */
typedef struct {
	const char *label;  /* legend text, e.g. "1 wk" */
	double max_age_s;   /* inclusive upper bound of this bucket's file
	                      * age, in seconds; meaningless for the last
	                      * entry (index FSN_AGE_BUCKET_COUNT - 1),
	                      * which is the catch-all "> 1 yr" bucket for
	                      * any age past the second-to-last cutoff */
	float rgb[3];        /* linear 0..1, fed straight into
	                       * gpu_set_color( )/ImGui color widgets --
	                       * same convention as FsnLandscape's fields */
} FsnAgeBucket;

#define FSN_AGE_BUCKET_COUNT 7

/* Colors sampled directly from the reference screenshot's legend
 * swatches (task-A2-brief.md's 35037135976_0d90f4a3d5_z.jpg), not
 * eyeballed by look alone: cropped the "ages:" bar (approx.
 * x=148..284, y=497..506) and took the per-channel median pixel value
 * over each swatch's x-range in Python/PIL, which is fairly robust
 * against the white bold-text glyphs and JPEG ringing sitting on top of
 * each swatch's flat fill. Rounded to a clean-looking value near each
 * median, same spirit as fsn_landscapes[] above.
 *
 * Note on the last bucket: the brief's own first-pass guess (before
 * this sampling) called it "grey" -- that guess does not survive a
 * close look at the actual pixels (median ~(87,44,68), and a 8x crop of
 * just that swatch shows a visibly dark plum/wine color, not a neutral
 * grey) or the plausible IRIS GL palette (a `-1` "no more buckets"
 * sentinel rendered as an increasingly dark, increasingly desaturated
 * continuation of the purple hue used one bucket up, rather than a
 * jump to a completely different, achromatic color). Corrected to the
 * sampled dark plum. */
static const FsnAgeBucket fsn_age_buckets[FSN_AGE_BUCKET_COUNT] = {
	{ "1 wk",   7.0 * 86400.0, { 0.565f, 0.235f, 0.208f } }, /* maroon-red, ~(144,60,53) */
	{ "2 wk",  14.0 * 86400.0, { 0.510f, 0.337f, 0.149f } }, /* orange-brown, ~(130,86,38) */
	{ "1 mo",  30.0 * 86400.0, { 0.569f, 0.561f, 0.153f } }, /* olive-yellow, ~(145,143,39) */
	{ "3 mo",  91.0 * 86400.0, { 0.225f, 0.375f, 0.361f } }, /* dark teal-green, ~(57,96,92) */
	{ "6 mo", 182.0 * 86400.0, { 0.212f, 0.290f, 0.551f } }, /* deep blue, ~(54,74,141) */
	{ "1 yr", 365.0 * 86400.0, { 0.404f, 0.192f, 0.518f } }, /* purple, ~(103,49,132) */
	{ "> 1 yr",          -1.0, { 0.341f, 0.173f, 0.269f } }, /* dark plum (catch-all; max_age_s unused), ~(87,44,68) */
};

/**** Landscape layout (fsn-mode Task B1, src/geometry-fsn.c) ****/

/* World units. fsv has no unit system of its own: MapV derives every
 * footprint from sqrt(bytes) and TreeV from a fixed 256-unit leaf edge
 * (TREEV_LEAF_NODE_EDGE, src/geometry.h), so "one unit" is whatever each
 * mode says it is. FSN picks a fixed absolute scale -- a file box is
 * always FSN_BOX_EDGE across, however big the file is; only its *height*
 * carries the size -- and deliberately stays in the same order of
 * magnitude as the other two modes (tens to hundreds of units), so the
 * shared camera near/far clip ratios (NEAR_TO_DISTANCE_RATIO /
 * FAR_TO_NEAR_RATIO, src/camera.h) keep working unchanged.
 *
 * All eyeballed from the reference screenshot (task-B1-brief.md's
 * 3060c037-069f-4715-a01e-c30e53e505a2.jpg): a directory is a wide, low
 * slab carrying a tight grid of small boxes, sibling directories sit
 * roughly one slab-width apart, and each generation is several
 * slab-depths further from the camera than the last. */
#define FSN_BOX_EDGE            64.0  /* file box footprint, square */
#define FSN_BOX_GAP             24.0  /* gap between adjacent file boxes */
#define FSN_PEDESTAL_MARGIN     48.0  /* clear band around the box grid;
                                       * also where the name label goes */
#define FSN_PEDESTAL_MIN_EDGE  192.0  /* floor for an empty directory, so
                                       * it is still a visible target */
#define FSN_SIBLING_GAP        320.0  /* clear space between the subtree
                                       * spans of two sibling directories */
#define FSN_GENERATION_GAP    1024.0  /* clear space between a parent's
                                       * outward edge and its children's
                                       * inward edge -- the wire length */

/* Pedestal height:
 *     h = MIN + SCALE * log2(1 + subtree_bytes / FSN_SIZE_UNIT), clamped
 * A real source tree spans five or six orders of magnitude of subtree
 * size; linear or even sqrt scaling makes the root pedestal tower so far
 * over its children that nothing else is legible in the same frame (the
 * reference screenshot's pedestals are all within a small factor of each
 * other). Log2 compresses that to a usable range, and the MAX clamp
 * bounds the pathological case (a multi-TB root) outright. */
#define FSN_SIZE_UNIT         1024.0  /* one "size step" is one kilobyte */
#define FSN_PEDESTAL_H_MIN      24.0
#define FSN_PEDESTAL_H_SCALE    20.0
#define FSN_PEDESTAL_H_MAX     512.0

/* File box height: same log law, with a gentler slope and a lower
 * ceiling than the pedestals above, so a directory full of large files
 * still reads as a slab carrying boxes rather than as a thicket.
 *
 * Note this is a tendency, not a guarantee: FSN_BOX_H_MAX (320) is well
 * above FSN_PEDESTAL_H_MIN (24), so a big file on a small directory's
 * pedestal genuinely does tower over it. That is honest -- the box
 * height *is* the file's size, and clamping it against its pedestal
 * would make two equal files render at different heights depending on
 * which directory they sit in, which is worse. */
#define FSN_BOX_H_MIN           16.0
#define FSN_BOX_H_SCALE         12.0
#define FSN_BOX_H_MAX          320.0

/* Directory-to-child wire: thin, bright, unlit. Eyeballed as plain white
 * in the reference screenshot; gpu_set_line_width() is honored by the GTK
 * shim and ignored by the SDL_GPU backend (see src/gpu.h), so the SDL
 * frontend draws these one pixel wide -- which is what the reference's
 * hairline wires look like anyway. */
#define FSN_WIRE_R 1.0f
#define FSN_WIRE_G 1.0f
#define FSN_WIRE_B 1.0f
#define FSN_WIRE_WIDTH 1.0f

/**** Warp-lite: directory double-click fly-in (fsn-mode Task C4, src/camera.c) ****/

/* Upstream fsn's "warp": double-clicking a directory pedestal drops the
 * camera down onto it, landing low and close so the file-box grid fills
 * the view, rather than the wide establishing shot fsn_look_at( ) (an
 * ordinary single click) frames the whole pedestal with. Same low-pitch
 * language as camera.c's FSN_CAMERA_PHI -- its own comment cites the same
 * reference screenshot -- just aimed tight at one pedestal instead of the
 * whole landscape. No Search panel, no true in-directory paradigm: YAGNI,
 * out of scope for this task (see the task brief). */

/* Camera elevation for the landing pose. Higher than camera.c's own
 * FSN_CAMERA_PHI (15 degrees, that function's grazing establishing-shot
 * pitch): a low elevation here put the camera *between* two rows of
 * file boxes, staring down a canyon of box side-walls rather than across
 * their tops (confirmed empirically -- a first pass at 8 degrees, with
 * the target sitting at the bare pedestal surface, produced exactly
 * that canyon). This -- together with FSN_WARP_HEIGHT_LIFT below -- is
 * what clears the camera over the box canopy instead of threading it
 * through the aisle between two rows. */
#define FSN_WARP_PHI           18.0

/* World units the look-at target is raised above the pedestal's own top
 * (FsnPedestal::h) -- aiming at roughly file-box height instead of the
 * bare pedestal surface the boxes stand on. Between FSN_BOX_H_MIN (16)
 * and FSN_BOX_H_MAX (320): a fixed guess at "typical" box height, not a
 * per-box lookup (this function has no per-child geometry, only the
 * parent pedestal's own FsnPedestal -- see fsn_warp_pose( )). */
#define FSN_WARP_HEIGHT_LIFT  110.0

/* How much of the pedestal's own footprint (MAX(w, d)) the frame is
 * sized to fit, vs. fsn_look_at( )'s SQRT_2 * MAX(w, d) which frames the
 * *whole* footprint (and, for an expanded directory, the next
 * generation's wires too). Under 1.0 so the near boxes fill the view
 * instead of the pedestal being seen whole from a diagonal, but not so
 * tight that the elevated camera above ends up past the near edge of
 * the box grid it is supposed to be looking across. */
#define FSN_WARP_DIAMETER_FRAC  0.22

/**** Flight navigation (fsn-mode Task B2, src/camera.c) ****/

/* fsn's signature gesture: hold the middle button and the pointer's
 * offset from the press point becomes a *velocity*, not a position --
 * push forward to fly forward, sideways to turn, Shift to climb/dive.
 * (README.txt; SGI patent US5555354 describes the same velocity model.)
 * Nothing here decelerates on approach to a target: that is the
 * click-to-fly half of the patent, which fsv already has in the form of
 * camera_look_at( )'s MORPH_SIGMOID pan.
 *
 * OFFSETS ARE IN FRAMEBUFFER PIXELS, not logical window points --
 * src/sdl/input.cpp works in pixel space throughout (its pixel_scale( )
 * helper), and its two existing gestures (MOUSE_SENSITIVITY-scaled dolly
 * and revolve) are already expressed there. So is this, for consistency;
 * the cost is that a given physical drag distance flies twice as fast on
 * a 2x display as on a 1x one, exactly as it already dollies twice as
 * fast today. Worth revisiting for all three gestures at once, not for
 * this one alone.
 *
 * All values eyeballed, then checked against this repo's own src/ tree
 * (see docs/PORTING.md's Task B2 note for the measured landscape size
 * and the resulting traversal time). */

/* Offset magnitude, in pixels, below which the axis reads as zero. A
 * click is never perfectly still -- without this, pressing the middle
 * button and letting go a moment later leaves the camera visibly
 * drifting. Small enough that a deliberate nudge still registers. */
#define FSN_FLIGHT_DEAD_ZONE_PX     6.0

/* Rate per pixel of offset past the dead zone, and the ceiling each rate
 * is clamped to. The ceilings are the numbers that matter (they set how
 * fast a full-deflection drag flies); the scales just say how far you
 * have to drag to get there -- e.g. 640/4.0 = 160px past the dead zone
 * for full speed, a comfortable drag inside any viewport. */
#define FSN_FLIGHT_SPEED_SCALE      4.0    /* world units/s per pixel */
#define FSN_FLIGHT_SPEED_MAX      640.0    /* world units/s */
#define FSN_FLIGHT_YAW_SCALE        0.45   /* degrees/s per pixel */
#define FSN_FLIGHT_YAW_MAX         72.0    /* degrees/s (5s for a full turn) */
#define FSN_FLIGHT_ALT_SCALE        2.0    /* world units/s per pixel */
#define FSN_FLIGHT_ALT_MAX        320.0    /* world units/s */

/* Longest time step a single flight tick will integrate over, in
 * seconds. The frame loop is not the only thing that can stall between
 * two ticks (a filesystem scan, a modal folder dialog, a debugger); an
 * unclamped dt would then teleport the viewer clear across the
 * landscape in one frame. ~4 frames at 60Hz. */
#define FSN_FLIGHT_MAX_STEP         0.0667

/* Text. The reference screenshot puts the current path on the ground in
 * front of the root pedestal in large outlined white letters; node name
 * labels are small and dark, like MapV's. Both are drawn flat (in the
 * world x/y plane, text_draw_straight( )), lifted FSN_TEXT_LIFT units off
 * whatever surface they label so they never z-fight with it. */
#define FSN_TEXT_LIFT            1.0
/* The ground path text is sized *relative to the root pedestal*, not in
 * absolute units: the camera frames the whole landscape, so a fixed cap
 * height reads as gigantic on a small tree and as a smudge on a large
 * one. Both ratios are of the root pedestal's width. */
#define FSN_PATH_TEXT_WIDTH_RATIO   2.0  /* text box width */
#define FSN_PATH_TEXT_HEIGHT_RATIO  0.10 /* cap height */
#define FSN_PATH_TEXT_GAP_RATIO     0.15 /* distance in front of the pedestal */
#define FSN_PATH_R 1.0f
#define FSN_PATH_G 1.0f
#define FSN_PATH_B 1.0f

/**** Selection spotlight (fsn-mode Task B3, src/geometry-fsn-draw.c) ****/

/* The SGI patent's (US5861885) literal ground-glow under the selected
 * node -- reference screenshot task-B3-brief.md points at
 * (35037135976_0d90f4a3d5_z.jpg): a soft white elliptical light pool on
 * the surface directly beneath the selected file box.
 *
 * DEPARTURE, same shape as Task A1's banded sky (see fsn_landscapes[]'s
 * own history): the brief's ideal is a true per-vertex alpha gradient
 * (center opaque, rim transparent) on a single fan. FsvVertex (src/gpu.h)
 * carries only position and normal -- no per-vertex color channel -- so
 * that gradient is not expressible through gpu_draw() as it stands, and
 * adding one purely for a decorative ground decal would be exactly the
 * kind of contract growth the task brief asks to avoid. Instead this
 * draws FSN_SPOTLIGHT_RING_COUNT concentric filled ellipses, largest
 * (faintest) first, each a flat FsvVertex fan at a single uniform alpha;
 * painted back-to-front with FSV_DEPTH_LESS_NOWRITE's alpha blending,
 * standard "over" compositing accumulates them into a stepped
 * approximation of the target falloff -- the per-layer alphas below were
 * chosen (and hand-verified via the over-compositing formula) so the
 * composited alpha at the center comes out near FSN_SPOTLIGHT_ALPHA_CENTER
 * while the outermost ring stays faint enough that its hard edge at
 * radius_frac 1.0 reads as a soft boundary rather than a visible ring. */
#define FSN_SPOTLIGHT_ALPHA_CENTER 0.55f /* documentation only -- see the
                                           * table below, which is what
                                           * the code actually reads */
#define FSN_SPOTLIGHT_SEGMENTS 32        /* fan resolution per ring */
#define FSN_SPOTLIGHT_RING_COUNT 6

typedef struct {
	double radius_frac; /* of the ellipse's full radius, outermost first */
	float alpha;        /* this layer's own (draw-order) alpha */
} FsnSpotlightRing;

/* Composited alpha at each band, outermost to innermost, drawing in this
 * order over a transparent background: 0.05, 0.12, 0.20, 0.29, 0.41,
 * 0.52 -- close enough to FSN_SPOTLIGHT_ALPHA_CENTER at the center given
 * everything here is eyeballed already. */
static const FsnSpotlightRing fsn_spotlight_rings[FSN_SPOTLIGHT_RING_COUNT] = {
	{ 1.00, 0.05f },
	{ 0.80, 0.07f },
	{ 0.60, 0.09f },
	{ 0.40, 0.12f },
	{ 0.20, 0.16f },
	{ 0.08, 0.20f },
};

/* Ellipse size, as a multiple of the selected node's own footprint --
 * task-B3-brief.md's "~2x the file box footprint" for a file, "around its
 * pedestal" (snugger -- a pedestal is already much larger than a file
 * box) for a directory. */
#define FSN_SPOTLIGHT_FILE_SCALE 2.00
#define FSN_SPOTLIGHT_DIR_SCALE  1.15

/* Height above the surface the decal sits on -- a directory's true
 * ground (world z = 0) or a file's parent pedestal top (world z ==
 * parent->h) -- same idea as FSN_TEXT_LIFT and deliberately a different,
 * smaller constant: the two lifts avoid z-fighting with different things
 * (a label sits on top of solid geometry it never overlaps in x/y; the
 * spotlight is a large translucent disc that often *does* overlap the
 * pedestal/box footprint it surrounds) and nothing ties them together. */
#define FSN_SPOTLIGHT_LIFT 0.5

/* Task B3 follow-up (user QA, 2026-08-14): the ground pool alone reads
 * as a faint rim -- white at ~0.55 composited alpha on a light-grey
 * pedestal top is structurally low-contrast, and in a packed box row
 * the ellipse is mostly hidden under the very boxes it surrounds. The
 * visible light CONE above the selection (US5861885's actual spotlight:
 * the beam, not just the pool it casts) silhouettes against sky,
 * landscape and pedestal faces instead of the surface it sits on, so it
 * stays legible at fsn_look_at( )'s parent-distance establishing shot.
 * Same alpha-decal rules as the rings (unlit, FSV_DEPTH_LESS_NOWRITE,
 * drawn only by fsn_draw_spotlight( ) so it inherits every guard); the
 * scene pipeline back-face culls, so only the beam's near half draws --
 * a single translucent layer, which is the wanted look. Starting values
 * below were tuned against --screenshot captures of this repo's own
 * src/ tree (see the plan doc); treat them as eyeballed, like the ring
 * table above. */
#define FSN_SPOTLIGHT_CONE_ALPHA       0.14f /* the beam's one visible layer */
#define FSN_SPOTLIGHT_CONE_MIN_HEIGHT  384.0 /* floor, world units above base */
#define FSN_SPOTLIGHT_CONE_HEIGHT_MULT 3.0   /* x the node's own height */
#define FSN_SPOTLIGHT_CONE_APEX_FRAC   0.05  /* apex ellipse : base ellipse --
                                               * near-pointed, so the
                                               * truncated top reads as a
                                               * beam converging from
                                               * above rather than a
                                               * flat-topped wedge with a
                                               * hard floating edge */

/* Fade-on-entry (TODO.md UX ticket): back-face culling makes the beam
 * vanish the instant the camera crosses the cone wall -- warp-lite
 * parks the camera exactly there. Fade the beam's alpha over a band
 * of the ratio (camera horizontal distance to the cone axis) /
 * (cone radius at the camera's height): 1 outside, 0 well inside,
 * smoothstep between, so the beam dissolves on approach instead of
 * snapping off. */
#define FSN_SPOTLIGHT_CONE_FADE_OUTER 1.15 /* ratio at which fade begins */
#define FSN_SPOTLIGHT_CONE_FADE_INNER 0.85 /* ratio at which alpha hits 0 */

#endif /* FSV_FSN_STYLE_H */
