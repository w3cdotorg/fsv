# Theta Unwrap + Night Preset Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close TODO.md's last two Known bugs: the "long way round" theta spin in DiscV/MapV/TreeV after a manual revolve, and the never-calibrated "Night" landscape preset.

**Architecture:** (1) Theta: `camera_revolve()` accumulates `camera->theta` without wrapping; every fixed-heading re-pose (`camera_birdseye_view()`'s going-up MapV/TreeV arms, its going-down restore — currently guarded `== FSV_FSN` — and the fixed-theta sites in `discv_look_at()`/`mapv_look_at()`/`treev_look_at()`) morphs the raw number, spinning up to a full turn. The fix is the existing `unwrap_theta_toward()` (src/camera.c:850, already proven at four FSN sites) applied at each such site, TDD'd headlessly (camera.c is in libfsvcore; the test simulates a revolve by setting `camera->theta` directly, snaps morphs via `morph_finish()` + `fsv_animation_tick()`, and asserts the short arc). (2) Night: `fsn_landscapes[1]`'s two sky values are documented guesses ("guess, uncalibrated"); calibrate them by the proven screenshot loop (persist `landscape=night` into `~/.fsvrc` with backup/restore, `--fsn --screenshot`, read, iterate). The ground stays classic-green ON PURPOSE: the overview mini-map is pinned to the night palette and its reference-green background comes from night's ground (fsn-style.h:55-59's own comment) — sky-only calibration preserves that.

**Tech Stack:** C89 GLib C, Meson/ninja, headless test recipe (libfsvcore + stubs + fsv_headless_platform_init), --screenshot visual loop.

**Spec:** `TODO.md` Known bugs items 3 and 4 (the last two unchecked) + `src/camera.c:2010-2021` (the going-down arm's own comment documenting the left-open behavior) + `src/fsn-style.h:87-101` (night's "flag for correction" comment).

## Global Constraints

- Build dir `builddir`; `export PATH="/opt/homebrew/bin:$PATH"`; repo root `/Users/willow/Sites/_Claude_output/fsn/fsv`; branch `fsn-mode`. `-DDEBUG` live.
- C89 spaced `func( arg )` style in src/*.c and tests/. Commit per task, why-bodies.
- Suite is currently 8 tests; Task 1 brings it to 9.
- `unwrap_theta_toward( target )` semantics (read src/camera.c:830-860 first): it adjusts the CURRENT `camera->theta` by ±360-multiples so the coming morph to `target` takes the short arc — call it immediately BEFORE scheduling the theta morph, exactly as the four existing FSN call sites do.
- Any `~/.fsvrc` touched during Task 2's capture loop is backed up first and byte-restored after (proven pattern).
- Screenshots to `<plan workspace>/shots/`.

---

### Task 1: Short-arc theta on every fixed-heading re-pose, TDD

**Files:**
- Create: `tests/test_camera_theta.c`
- Modify: `tests/meson.build` (new entry, same recipe as `test_fsn_camera` — fixture + stubs)
- Modify: `src/camera.c` (the unwrap calls + comment updates; AFTER the failing test is recorded)

**Interfaces:**
- Consumes: `unwrap_theta_toward()` (static in camera.c — the FIX calls it; the TEST asserts observable theta, no new exports needed); `camera_birdseye_view()`, `camera_look_at()`, `camera` global, `morph_finish()`/`fsv_animation_tick()`, `fsv_headless_platform_init()`, stateful dirtree stubs.
- Produces: meson test `camera_theta`.

- [ ] **Step 1: Read the current sites**

Map every fixed-theta re-pose in the three modes before writing anything: `camera_birdseye_view()`'s going-up arms (`src/camera.c:1957-1990`: MapV sets 270.0, TreeV sets `90.0 - target.theta`, DiscV sets NO theta — check whether DiscV's camera math reads theta at all; if it is inert there, leave the DiscV up-arm alone and say so in a code comment), its going-down restore (`:2010-2021`, the `== FSV_FSN` guard), and the `discv_look_at()`/`mapv_look_at()`/`treev_look_at()` helpers (grep `->theta =` within them) for fixed-heading assignments followed by `morph( &camera->theta, … )`.

- [ ] **Step 2: Write the failing test**

Create `tests/test_camera_theta.c`:

```c
/* tests/test_camera_theta.c
 *
 * SPDX-License-Identifier: MIT
 *
 * Short-arc theta on fixed-heading re-poses (TODO.md's last Known bug):
 * camera_revolve( ) accumulates camera->theta without wrapping, and
 * morph( ) interpolates raw numbers, not angles -- so a bird's-eye hop
 * (or a look-at) after a manual revolve used to spin the long way
 * round to its fixed heading in DiscV/MapV/TreeV. FSV_FSN got
 * unwrap_theta_toward( ) in fsn-mode Task B2; this locks the same
 * treatment onto the other modes' re-pose sites.
 *
 * The revolve is simulated by assigning camera->theta directly (the
 * accumulation is the documented behavior under test's PREcondition,
 * not its subject), morphs are snapped via morph_finish( ) +
 * fsv_animation_tick( ) (same mechanics as test_fsn_framing), and the
 * assertion is on observable state only: the settled heading is
 * congruent to the target mod 360, and the total theta travel of the
 * morph never exceeded 180 degrees.
 *
 * LINKING: the standard libfsvcore + headless-stubs recipe. MapV's
 * geometry params are all zero headlessly (geometry.c never ran);
 * every quantity the exercised paths derive from them stays finite
 * (field_distance( ) is floored), and theta -- the subject -- does not
 * depend on layout at all. */

#include <assert.h>
#include <math.h>

#include "common.h" /* pulls in glib.h, and must precede it: G_LOG_DOMAIN */
#include "fsv.h"
#include "animation.h"
#include "camera.h"
#include "dirtree.h"
#include "scanfs.h"

#define TOL 1.0e-9

/* tools/fsv-headless-stubs.c -- opt-in fsv_platform no-op hooks */
extern void fsv_headless_platform_init( void );

static void
snap_theta_morph(void)
{
	morph_finish(&camera->theta);
	morph_finish(&camera->phi);
	morph_finish(&camera->distance);
	morph_finish(&camera->near_clip);
	morph_finish(&camera->far_clip);
	morph_finish(&camera->pan_part);
	fsv_animation_tick();
}

/* Smallest congruence distance between two headings, in degrees */
static double
angle_diff(double a, double b)
{
	double d = fmod(fabs(a - b), 360.0);

	return d > 180.0 ? 360.0 - d : d;
}

/* One scenario: in `mode`, park the camera at a wound-up heading
 * (simulating several manual revolves), fire the bird's-eye hop, snap,
 * and require (1) the settled heading is the mode's fixed bird's-eye
 * target mod 360 and (2) the morph's endpoints were never more than
 * 180 apart -- the short arc. Then return from bird's-eye and require
 * the same short-arc property on the restore. */
static void
check_birdseye_short_arc(FsvMode mode, double wound_theta,
    double expect_target_mod360)
{
	double before, after;

	globals.fsv_mode = mode;
	camera_init(mode, TRUE);
	snap_theta_morph(); /* settle any init pan */

	camera->theta = wound_theta;

	camera_birdseye_view(TRUE);
	before = camera->theta; /* unwrap may have adjusted it in place */
	snap_theta_morph();
	after = camera->theta;

	assert(angle_diff(after, expect_target_mod360) < TOL);
	assert(fabs(after - before) <= 180.0 + TOL);

	/* And back down: the restore must also take the short arc to the
	 * saved pre-bird's-eye pose (wound_theta itself, mod 360) */
	camera_birdseye_view(FALSE);
	before = camera->theta;
	snap_theta_morph();
	after = camera->theta;

	assert(angle_diff(after, wound_theta) < TOL);
	assert(fabs(after - before) <= 180.0 + TOL);
}

int
main(void)
{
	fsv_headless_platform_init();

	scanfs(FIXTURE_DIR);
	assert(globals.fstree != NULL && root_dnode != NULL);

	/* MapV bird's-eye heading is 270; park at 630 (= 270 + 360): the
	 * long way round is a full -360 sweep, the short arc is zero. */
	check_birdseye_short_arc(FSV_MAPV, 630.0, 270.0);

	/* And from a heading NOT congruent to the target: 610 -> nearest
	 * 270-congruent value is 630, a +20 short arc (the old code went
	 * 610 -> 270, a -340 sweep). */
	check_birdseye_short_arc(FSV_MAPV, 610.0, 270.0);

	/* TreeV: its bird's-eye heading depends on the camera's own
	 * target.theta (90 - target.theta); read the expectation from the
	 * live camera rather than hardcoding it. */
	{
		double expect;

		globals.fsv_mode = FSV_TREEV;
		camera_init(FSV_TREEV, TRUE);
		snap_theta_morph();
		expect = 90.0 - TREEV_CAMERA(camera)->target.theta;
		camera->theta = expect + 360.0 + 20.0; /* wound up a turn + 20 */

		camera_birdseye_view(TRUE);
		{
			double before = camera->theta, after;

			snap_theta_morph();
			after = camera->theta;
			assert(angle_diff(after, expect) < TOL);
			assert(fabs(after - before) <= 180.0 + TOL);
		}
		camera_birdseye_view(FALSE);
		snap_theta_morph();
	}

	return 0;
}
```

Meson entry (standard recipe, fixture needed):

```meson
# Short-arc theta on fixed-heading re-poses after a manual revolve
# (TODO.md's last Known bug): DiscV/MapV/TreeV get the same
# unwrap_theta_toward() treatment FSN's Task B2 fix round introduced.
test_camera_theta = executable('test_camera_theta',
  ['test_camera_theta.c', headless_stubs_src],
  objects: libfsvcore.extract_all_objects(recursive: false),
  dependencies: core_test_deps,
  include_directories: incdir,
  c_args: fixture_c_args)
test('camera_theta', test_camera_theta)
```

- [ ] **Step 3: Run — must FAIL (RED)** on a short-arc assertion (the `fabs(after - before) <= 180` one, in the 610→270 MapV case at minimum). If it fails EARLIER (setup: camera_init snap, TreeV target read), fix the TEST until the failure is exactly a short-arc assert; record the output. Do not touch camera.c yet.

- [ ] **Step 4: Implement the fix (GREEN)**

In `src/camera.c`:
1. Going-up arms: add `unwrap_theta_toward( new_cam->theta );` after the theta assignment in the MapV and TreeV cases (mirroring the FSN case's existing call and its one-line comment). DiscV: per Step 1's finding — if its up-arm sets no theta and theta is inert in DiscV pose math, leave it and extend the arm's comment to say why.
2. Going-down restore: replace the `if (globals.fsv_mode == FSV_FSN)` guard with an unconditional call, and rewrite the accompanying block comment (`:2010-2021`): the "DiscV/MapV/TreeV never wrap theta the way a flight does" rationale was half-true — a MANUAL REVOLVE winds theta in every mode, and this was TODO.md's documented left-open bug, now closed.
3. `discv_look_at()`/`mapv_look_at()`/`treev_look_at()`: add `unwrap_theta_toward( new_cam->theta );` immediately before each helper's theta morph (same placement as `fsn_look_at()`'s at `:1310`), for every fixed-heading assignment found in Step 1. (Same bug class, same one-line fix; leaving look-at spinning while bird's-eye is fixed would be a half-fix a user notices immediately.)

- [ ] **Step 5: Run — GREEN, then whole suite** → `meson test -C builddir camera_theta --print-errorlogs && meson test -C builddir` → 9/9 (the FSN tests must be untouched by the now-unconditional going-down unwrap: FSN already had it).

- [ ] **Step 6: Commit**

```bash
git add tests/test_camera_theta.c tests/meson.build src/camera.c
git commit -m "fix: short-arc theta on DiscV/MapV/TreeV re-poses after a revolve

camera_revolve() winds camera->theta without wrapping and morph()
interpolates raw numbers, so any fixed-heading re-pose -- bird's-eye
up, its restore, or a look-at -- could spin the long way round in the
three non-FSN modes (TODO.md's documented left-open half of the
fsn-mode Task B2 fix). Apply the same unwrap_theta_toward() at every
such site: bird's-eye going-up MapV/TreeV arms, the going-down
restore (guard dropped -- a manual revolve winds theta in every mode,
not only after an FSN flight), and the fixed-heading assignments in
the three modes' look_at helpers. TDD'd headlessly: test_camera_theta
winds the heading, snaps the morphs, asserts congruence mod 360 plus
a <=180-degree travel bound, both directions of the bird's-eye hop."
```

---

### Task 2: Calibrate the Night landscape sky

**Files:**
- Modify: `src/fsn-style.h` (the night entry's two sky values + its comment block)
- Temporary (never committed): `~/.fsvrc` landscape key during captures (backup/byte-restore)

**Interfaces:**
- Consumes: `fsn_landscapes[FSN_LANDSCAPE_NIGHT]` (`src/fsn-style.h:87-101`), nvstore `landscape` token key (`src/color.c:117-127`, token `"night"`), `--fsn --screenshot`.
- Produces: calibrated sky_top/sky_horizon values; the "guess, uncalibrated" flags removed.

- [ ] **Step 1: Baseline capture**

Backup `~/.fsvrc`. Write `landscape=night` + `landscape_explicit=true` into it using the token key exactly as color.c reads it (check `landscape_write_config()` for the precise key/value shapes — safest: run the app once and set it via nvstore-compatible hand edit matching an existing written file, or write a 5-line scratch C harness using lib/nvstore like Task 3 of the MapV plan did). Capture `<shots>/night-before.png`: `builddir/src/sdl/fsv --fsn --screenshot <shots>/night-before.png src`. READ it (convert BMP-under-.png via sips if needed).

- [ ] **Step 2: Tune the sky, iterating on captures**

Constraints that define "calibrated" here (the honest scope — no night reference screenshot exists, fsn-style.h's own comment says so):
1. The sky must read as NIGHT (dark zenith, subtle horizon glow) yet keep the horizon boundary visible against the ground.
2. Wires (white), box labels (dark text on pale boxes) and the age-spectrum boxes must all stay legible — check a shot with an expanded directory in frame (`FSV_TEST-style selection isn't needed; the default framing shows the landscape`).
3. The spotlight beam (white, alpha 0.14) must remain visible against the night sky — capture one shot with a selection if feasible; if selection needs a harness, judge beam-vs-sky contrast arithmetically instead (beam adds 0.14 white over sky_top: compute the contrast and state it).
4. The GROUND VALUE DOES NOT CHANGE — the overview mini-map is pinned to the night palette and its reference-green background is night's ground (extend the existing in-table comment to say the pin makes the ground load-bearing for the overview, not just aesthetic).
Starting suggestion (tune from here): sky_top `{ 0.012f, 0.02f, 0.09f }` (deep blue, not pure black — pure black kills the horizon gradient), sky_horizon `{ 0.15f, 0.20f, 0.38f }` (moonlit glow). Save iterations as `<shots>/night-N.png`; final as `<shots>/night-after.png`.

- [ ] **Step 3: Update the comment block**

Rewrite the night entry's comment: no longer "guess, uncalibrated / flag for correction" — now "calibrated by screenshot iteration (2026-08, plan 2026-08-14-theta-unwrap-night-preset.md) against the legibility constraints there: night sky with a readable horizon, wires/labels/age colors legible, spotlight beam visible; ground deliberately unchanged (the overview mini-map is pinned to this palette and its background is this ground)."

- [ ] **Step 4: Restore ~/.fsvrc, build, suite, commit**

Byte-restore the config backup, confirm with `diff`. `ninja -C builddir && meson test -C builddir` → 9/9.

```bash
git add src/fsn-style.h
git commit -m "fix(fsn): calibrate the Night landscape sky

The night preset shipped as a documented guess (no night reference
screenshot exists -- both Task A1 references are daylight). Calibrated
by the screenshot loop against explicit legibility constraints: dark
zenith with a readable horizon gradient, wires/labels/age spectrum
legible, spotlight beam visible against the sky. The ground stays
classic green on purpose -- the overview mini-map is pinned to this
palette and its reference-green background IS this ground value."
```

---

### Task 3: Close the tickets, push

**Files:**
- Modify: `TODO.md` (the two Known-bugs items → checked closures)
- Modify: `docs/PORTING.md` (the Task B2 fix-round note that documented the left-open theta behavior — grep `long way round` — gets a closure sentence; the B3/A1 night mention if any)
- Commit also: this plan file `docs/superpowers/plans/2026-08-14-theta-unwrap-night-preset.md`

- [ ] **Step 1:** TODO.md: replace both items with checked past-tense closures in the file's voice, citing `unwrap_theta_toward()`'s new sites + meson test `camera_theta`, and the calibrated night values + the overview-pin rationale for the unchanged ground. The Known-bugs section is then fully closed — note that in the section if the file's voice suits it.
- [ ] **Step 2:** PORTING.md: one closure sentence at the `long way round` fix-round note ("other modes closed post-v0.3, see camera_theta test") and, if the A1/B3 sections flag night as uncalibrated, the matching update.
- [ ] **Step 3:** Commit + push:

```bash
git add TODO.md docs/PORTING.md docs/superpowers/plans/2026-08-14-theta-unwrap-night-preset.md
git commit -m "docs: close the last two Known bugs (theta unwrap, night calibration)

The Known-bugs section of TODO.md is now fully closed; point both
tickets at their fixes and tests, and sync PORTING.md's fix-round
notes that had documented the left-open halves."
git push origin fsn-mode
```

Watch CI to green (`gh run list -R w3cdotorg/fsv --branch fsn-mode --limit 1`, `gh run watch <id> -R w3cdotorg/fsv --exit-status`). If CI fails, report — do not attempt fixes.

---

## Notes for the final review

- Task 1's DiscV finding (theta inert or not in its up-arm) must be resolved with a code citation, not assumed.
- Task 2's evidence is `night-before.png` / `night-after.png` — read them; the beam-contrast constraint may be arithmetic rather than visual (disclosed above).
- The plan file itself is committed in Task 3.
