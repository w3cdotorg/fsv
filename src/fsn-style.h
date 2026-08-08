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

#endif /* FSV_FSN_STYLE_H */
