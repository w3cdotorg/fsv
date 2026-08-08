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
		 * nothing to sample a pixel from. Kept close to the plan's
		 * original guess (darkened classic, starless) for lack of
		 * anything better; flag for correction once real reference
		 * material (or hardware) turns up. */
		"night",
		{ 0.01f, 0.01f, 0.03f }, /* sky_top: near-black starless zenith (guess, uncalibrated) */
		{ 0.10f, 0.12f, 0.25f }, /* sky_horizon: dim blue-violet glow (guess, uncalibrated) */
		{ 0.255f, 0.549f, 0.353f }, /* ground: same green as classic -- the
		                             * ground doesn't relight at night any
		                             * more than the overview window's own
		                             * green background does (reference
		                             * screenshot 1's inset thumbnail) */
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

#endif /* FSV_FSN_STYLE_H */
