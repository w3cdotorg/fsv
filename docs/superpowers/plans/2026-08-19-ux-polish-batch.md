# UX Polish Batch Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close four "UX rough edges" tickets in one batch: glob-pattern scan exclusion + `--exclude` CLI (user's design choice, 2026-08-19), spotlight-beam fade-on-entry, overview mini-map zoom-out-to-include-camera (capped), and Marks de-duplication.

**Architecture:** Four small, independent fixes. (1) `scanfs.c`'s `dir_name_excluded()` moves from `strcmp` to `fnmatch()` (POSIX, no new deps); the built-in list becomes patterns (`builddir*`/`.builddir*` replace the two exact names — this is what catches the user's own `builddir-sdl`/`builddir-gtk`); a `scanfs_add_exclude_pattern()` API feeds a repeatable `--exclude PATTERN` flag parsed in `src/sdl/main.cpp`. TDD via the existing `scanfs_exclude` test. (2) `fsn_draw_spotlight()`'s cone gets the ticket's own designed fix: fade the beam's alpha as the camera's horizontal distance to the cone axis drops inside the cone's radius at camera height (back-face culling currently snaps it invisible; warp-lite lands the camera there). Visual verification with inside/edge/outside captures. (3) `gpu.cpp`'s `overview_frame_scene()` frames the union of the landscape bounds and the camera's ground position, with a zoom-out cap (beyond the cap, today's edge-clamp marker behavior remains as the fallback). Visual verification. (4) `ui_rail.cpp`'s "Mark here" skips when a mark with the same node path already exists. Docs close all four tickets.

**Tech Stack:** C89 GLib C (core), C++ SDL/ImGui (frontend), fnmatch(3), Meson/ninja, headless test + `--screenshot`/throwaway-harness visual verification (established conventions).

**Spec:** `TODO.md`'s four open UX tickets (grep: "exact-basename-only", "vanishes when the camera is inside", "framing ignores the camera", "no de-duplication on \"Mark here\"") + the user's two design choices (AskUserQuestion 2026-08-19 ~11:32): built-in globs AND `--exclude` CLI, NO editable UI list; mini-map zooms out with a cap.

## Global Constraints

- Build dir `builddir`; `export PATH="/opt/homebrew/bin:$PATH"`; repo root `/Users/willow/Sites/_Claude_output/fsn/fsv`; branch `fsn-mode`; BASE `6fc278f`. `-DDEBUG` live. Suite currently 9 tests (stays 9 — Task 1 extends `test_scanfs_exclude`, no new executable).
- C89 spaced `func( arg )` style in `src/*.c`; frontend style in `.cpp`. Commit per task, why-bodies. ≤5 files/task.
- Screenshots to `<plan workspace>/shots/`; BMP-under-.png → sips convert before reading; any throwaway harness (FSV_TEST_*) removed before commit (`git diff` clean on its file); `~/.fsvrc` byte-restored if touched.
- Pattern semantics (Task 1, binding): `fnmatch( pattern, name, 0 )` on the directory BASENAME, directories only, case-sensitive, no FNM_PATHNAME (no `/` in basenames anyway). Built-in pattern list V2: `.git` `.svn` `.hg` `node_modules` `__pycache__` `.venv` `.cache` `builddir*` `.builddir*`. `FSV_NO_EXCLUDE` env override unchanged.

---

### Task 1: Glob exclusion + `--exclude` CLI, TDD

**Files:**
- Modify: `src/scanfs.c` (fnmatch matching, pattern list, `scanfs_add_exclude_pattern()`), `src/scanfs.h` (prototype)
- Modify: `src/sdl/main.cpp` (`--exclude PATTERN` repeatable flag + usage string)
- Modify: `src/sdl/ui_main.cpp` (the Vis-toggle tooltip: list becomes patterns + mentions `--exclude`)
- Modify: `tests/test_scanfs_exclude.c` (new cases)

**Interfaces:**
- Produces: `void scanfs_add_exclude_pattern( const char *pattern );` — copies the string (callers may pass argv memory, which outlives the app anyway, but copy for safety with `g_strdup`); appended patterns participate identically to built-ins (same `FSV_NO_EXCLUDE` and toggle gating).

- [ ] **Step 1: Extend the test first (RED)**

In `tests/test_scanfs_exclude.c`'s temp-tree setup add: `builddir-sdl/` with a file (must be excluded by the `builddir*` glob), `buildding/` with a file (must NOT match `builddir*` — fnmatch is anchored, but this pins the semantics), and `secrets/` with a file (excluded only after `scanfs_add_exclude_pattern("secr*")`). New assertions, in order: with defaults ON — `builddir-sdl` absent, `buildding` present; then `scanfs_add_exclude_pattern("secr*"); scanfs(root);` — `secrets` absent, `buildding` still present; the existing OFF-case still shows everything (including `builddir-sdl` and `secrets` — the toggle gates user patterns too). Run: `meson test -C builddir scanfs_exclude` → FAIL on the `builddir-sdl` absence (exact-match code doesn't glob). Record.

- [ ] **Step 2: Implement**

`src/scanfs.c`: add `#include <fnmatch.h>`; the static table becomes `excluded_dir_patterns[]` with the V2 list (Global Constraints); add
```c
static GPtrArray *user_exclude_patterns = NULL;

void
scanfs_add_exclude_pattern( const char *pattern )
{
	if (pattern == NULL || pattern[0] == '\0')
		return;
	if (user_exclude_patterns == NULL)
		user_exclude_patterns = g_ptr_array_new( );
	g_ptr_array_add( user_exclude_patterns, g_strdup( pattern ) );
}
```
and `dir_name_excluded()` switches to `fnmatch( excluded_dir_patterns[i], name, 0 ) == 0` over both the built-in table and (if non-NULL) `user_exclude_patterns`. Comment updates: the list is now patterns (why `builddir*`: real checkouts name variants `builddir-sdl` etc.), the anchored-match semantics, and that user patterns ride the same enable/env gates.

`src/sdl/main.cpp` arg loop: `else if (strcmp(argv[i], "--exclude") == 0 && i + 1 < argc) scanfs_add_exclude_pattern(argv[++i]);` (+ `#include "scanfs.h"` if absent — grep first) + the usage() line.

`src/sdl/ui_main.cpp`: tooltip text → the pattern list verbatim + `"…plus any --exclude patterns"`.

- [ ] **Step 3: GREEN + suite** → `meson test -C builddir scanfs_exclude --print-errorlogs && meson test -C builddir` → 9/9.

- [ ] **Step 4: Commit**

```bash
git add src/scanfs.c src/scanfs.h src/sdl/main.cpp src/sdl/ui_main.cpp tests/test_scanfs_exclude.c
git commit -m "feat: glob patterns + --exclude for scan exclusion

Exact-basename matching left the everyday variants scanning -- this
repo's own builddir-sdl/builddir-gtk were the acceptance shot's
biggest blocks. The built-in list becomes fnmatch patterns
(builddir*/.builddir* replace the two exact names) and a repeatable
--exclude PATTERN flag feeds user patterns through the same gates
(the Vis toggle and FSV_NO_EXCLUDE cover them too). Anchored fnmatch
on the basename, directories only, no new dependencies. TDD'd:
builddir-sdl excluded by glob, 'buildding' pinned as a non-match,
a runtime secr* pattern, and the OFF case restores everything."
```

---

### Task 2: Spotlight beam fade-on-entry

**Files:**
- Modify: `src/fsn-style.h` (fade constants), `src/geometry-fsn-draw.c` (the fade in `fsn_draw_spotlight()`)
- Temporary (never committed): FSV_TEST harness in `src/sdl/main.cpp` for posed captures

**Interfaces:**
- Consumes: the cone draw (`fsn_gldraw_spotlight_cone()`, `FSN_SPOTLIGHT_CONE_*`), the camera's ground position — find how `src/sdl/ui_overview.cpp` derives the marker's ground position from the camera pose and reuse/mirror that derivation (if a shared accessor is worth extracting into camera.c, do it — PORTING.md's fsn-mode deferred notes already wish for a `camera_ground_position` dedup accessor; keep it small and C-linkage).

- [ ] **Step 1: Constants + fade math**

`src/fsn-style.h`, after the cone constants:
```c
/* Fade-on-entry (TODO.md UX ticket): back-face culling makes the beam
 * vanish the instant the camera crosses the cone wall -- warp-lite
 * parks the camera exactly there. Fade the beam's alpha over a band
 * of the ratio (camera horizontal distance to the cone axis) /
 * (cone radius at the camera's height): 1 outside, 0 well inside,
 * smoothstep between, so the beam dissolves on approach instead of
 * snapping off. */
#define FSN_SPOTLIGHT_CONE_FADE_OUTER 1.15 /* ratio at which fade begins */
#define FSN_SPOTLIGHT_CONE_FADE_INNER 0.85 /* ratio at which alpha hits 0 */
```
`src/geometry-fsn-draw.c`, in `fsn_draw_spotlight()` before the cone call: compute the camera's ground position and height (the reused/mirrored derivation above); `radius_at_cam = ` linear interpolation of the cone's x/z radii between base (rx/rz at cone_base_z) and apex (APEX_FRAC·rx at apex_z) at the camera's z, clamped to the cone's z span; `ratio = hypot( cam_x - cx, cam_y - cz ) / MAX(EPSILON, radius_at_cam)` using the larger of rx/rz consistently (an elliptical-exact test is overkill for a fade — say so in the comment); alpha factor = 0 below INNER, 1 above OUTER, smoothstep between; skip the cone draw entirely at factor 0. Pass the factor into the cone draw (add an `alpha_scale` parameter to `fsn_gldraw_spotlight_cone()` or scale `FSN_SPOTLIGHT_CONE_ALPHA` at the call — keep whichever reads cleaner in place). The ground POOL rings are not faded (they're visible from inside and correct).

- [ ] **Step 2: Visual verification (throwaway harness)**

FSV_TEST harness (same conventions as ever — env-gated in the `--screenshot` path, never committed): pose three captures with a directory selected — `fade-outside.png` (ordinary look-at framing: beam at full alpha, unchanged vs the pre-change look), `fade-edge.png` (camera near the cone wall — e.g. warp pose backed off: partial alpha, beam visibly translucent-er but present), `fade-inside.png` (warp pose: beam absent — same as today, but now by fade rather than cull-snap; nothing else changed). READ all three. If the edge pose is hard to hit, tune the pose or widen the band, and say what you did. Remove the harness (`git diff src/sdl/main.cpp` clean).

- [ ] **Step 3: Suite (9/9, no-breakage) + commit**

```bash
git add src/fsn-style.h src/geometry-fsn-draw.c
git commit -m "fix(fsn): fade the spotlight beam on cone entry instead of snapping off

Back-face culling made the beam vanish the frame the camera crossed
the cone wall, and warp-lite parks the camera exactly there -- the
selection indicator silently disappeared at close range (TODO.md UX
ticket, disclosed in the cone's own review). Fade the beam's alpha
over a smoothstep band of camera-distance-to-axis vs cone radius at
camera height, skipping the draw entirely once fully inside. The
ground pool is deliberately not faded (visible and correct from
inside). Verified with outside/edge/inside posed captures."
```

---

### Task 3: Overview mini-map zoom-out (capped)

**Files:**
- Modify: `src/fsn-style.h` (cap constant), `src/sdl/gpu.cpp` (`overview_frame_scene()`, ~line 1510)
- Possibly consume Task 2's ground-position accessor (same derivation).

**Interfaces:**
- Consumes: `fsn_layout_bounds()`, `FSN_OVERVIEW_MARGIN`, `overview_fit_aspect()`; camera ground position (Task 2's accessor if extracted, else the same local derivation gpu.cpp can reach).

- [ ] **Step 1: Constant + framing change**

`src/fsn-style.h`: `#define FSN_OVERVIEW_MAX_GROWTH 3.0 /* the framed rect may grow to at most this multiple of the landscape's own larger dimension to chase the camera; beyond that the marker edge-clamps as before */` (comment in the file's voice, referencing the ticket).
`gpu.cpp` `overview_frame_scene()`: after the margin step and before `overview_fit_aspect()`, extend `x0/x1/y0/y1` to include the camera's ground position (plus one margin) — but clamp the total extent to `FSN_OVERVIEW_MAX_GROWTH * MAX(landscape w, landscape h)` per axis, growing symmetrically toward the camera only (don't recenter the landscape needlessly: extend only the sides the camera is beyond). Update the function's comment: the frame now tracks the camera up to the cap; the marker's pre-existing edge-clamp remains the beyond-cap fallback (cite the ticket). Check `ui_overview.cpp`'s change-detection key (it hashes the camera pose already — line ~123) still triggers re-render when only the camera moves: it does (camera pose is in the key), note it.

- [ ] **Step 2: Visual verification**

Captures (overview needs ImGui → use `--record`'s compositing or judge via the live app? NO — established fact: `--screenshot` renders scene-only, the overview is ImGui. Use a short `--record` capture (2s, small) with a FSV_TEST harness posing the camera far outside the landscape, then extract a frame with ffmpeg and READ it: the mini-map must show the whole landscape smaller + the camera marker INSIDE the frame with visible separation; a second pose beyond the cap shows the marker back at the edge (clamped). Also one default-pose frame: framing unchanged (camera inside bounds → no growth). Remove the harness.

- [ ] **Step 3: Suite (9/9) + commit**

```bash
git add src/fsn-style.h src/sdl/gpu.cpp
git commit -m "fix(fsn): overview frame chases the camera, capped

The mini-map framed the landscape alone, so a camera outside it
pinned the marker to the frame edge with its distance unreadable
(TODO.md UX ticket, C1's own disclosed trade). The framed rect now
grows -- toward the camera only, up to FSN_OVERVIEW_MAX_GROWTH x the
landscape's larger dimension -- so the marker stays inside with
readable separation; beyond the cap the old edge-clamp remains the
fallback. Default poses are framed exactly as before (no growth when
the camera is over the landscape). Verified via --record frame
extraction at inside/outside/beyond-cap poses."
```

---

### Task 4: Marks de-dup + docs closure + push

**Files:**
- Modify: `src/sdl/ui_rail.cpp` (the "Mark here" handler, ~line 343)
- Modify: `TODO.md` (close/trim the four tickets), `docs/PORTING.md` (matching notes where those tickets cite disclosed gaps)
- Commit also: this plan file.

- [ ] **Step 1: De-dup**

In the "Mark here" button handler: before appending, walk the marks vector for an entry whose stored path equals `node_absname( globals.current_node )` (the same canonical form the rows store — read how add builds its path and compare THAT form); if found, do not add — instead set that row's rename-editing state (`g_editing_index`-style focus, matching the panel's existing single-editing-row mechanism) so the click still gives visible feedback. One comment citing the ticket.

- [ ] **Step 2: Docs**

TODO.md: check off the four tickets in the file's voice (exclusion ticket → globs + `--exclude` shipped, cite the test; beam fade; overview growth + cap; marks de-dup — trim the multi-part Marks ticket to its remaining parts if any (single-rename-at-a-time and the raw-path tooltip remain accepted-YAGNI: keep them listed, split the bullet)). PORTING.md: the C1/C2/B3-cone concern notes that disclosed these gaps get their closure sentences (grep each ticket's PORTING.md pointer).

- [ ] **Step 3: Suite, commit, push, CI**

```bash
git add src/sdl/ui_rail.cpp TODO.md docs/PORTING.md docs/superpowers/plans/2026-08-19-ux-polish-batch.md
git commit -m "fix(ui): de-dup Mark here; docs: close the UX polish batch tickets

Marking the same node twice created identical rows (Task C2's
disclosed YAGNI); Mark here now focuses the existing row's rename
field instead. Close the four batch tickets (glob exclusion +
--exclude, beam fade-on-entry, capped overview growth, marks de-dup)
and sync the PORTING.md notes that disclosed each gap."
git push origin fsn-mode
```
Watch CI green (`gh run watch <id> -R w3cdotorg/fsv --exit-status`). If CI fails, report — do not attempt fixes.

---

## Notes for the final review

- Visual evidence: fade-{outside,edge,inside}.png, the overview inside/outside/beyond-cap frames — read them.
- Task 2/3 share the camera-ground-position derivation; if Task 2 extracted an accessor, Task 3 must consume it (no second copy).
- The plan file is committed in Task 4.
