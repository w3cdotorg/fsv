# FSN Spotlight Cone Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the selection spotlight actually visible (user QA feedback, 2026-08-14) by adding US5861885's visible light cone above the selected node — a translucent beam from above — on top of the existing ground-pool decal.

**Architecture:** Root cause of the invisibility: the Task B3 spotlight is a white ground ellipse composited to ~0.55 alpha at center on a light-grey pedestal top (white-on-grey is structurally low-contrast), and in a packed box row the ellipse is mostly hidden under the very boxes it surrounds — only a faint rim escapes. The cone fixes the geometry of the problem rather than the palette: a truncated translucent cone from `FSN_SPOTLIGHT_CONE_*`-tuned height down to the existing spotlight ellipse silhouettes against the sky, the landscape, and pedestal faces instead of the surface it sits on, so it reads at `fsn_look_at()`'s parent-distance establishing shot. Drawn by the same `fsn_draw_spotlight()` (same select-pass/overview-pass/visibility guards, unlit, `FSV_DEPTH_LESS_NOWRITE` alpha decal). The scene pipeline back-face culls (GL default, `src/sdl/gpu.cpp:460-464`), so the strip shows its near half only — a single-layer beam, which is the wanted look; winding is visually verified during tuning. Verification is empirical, matching how Task B3 itself was verified: a throwaway env-var-gated `FSV_TEST_SELECT` block in `src/sdl/main.cpp`'s `--screenshot` path (this repo's established harness convention — Tasks A2/A3/B2/B3 — never committed), before/after screenshots saved to the plan workspace, constants tuned until legible.

**Tech Stack:** C (GLib) scene code, C++ SDL frontend (harness only, throwaway), Meson/ninja, `--screenshot` offscreen render (scene only, no ImGui — the spotlight is scene geometry, so it IS captured).

**Spec:** User QA report (2026-08-14, screenshot: `input.cpp` selected in `src/sdl`, spotlight nearly invisible) + `docs/PORTING.md` Task B3 section (US5861885 "selection spotlight"; the pool-only implementation and its disclosed self-occlusion) + user's explicit choice: the light cone (over halo punch-up or box highlight).

## Global Constraints

- Build dir `builddir`; `export PATH="/opt/homebrew/bin:$PATH"` first; repo root `/Users/willow/Sites/_Claude_output/fsn/fsv`; branch `fsn-mode`.
- C89-flavored GLib C in `src/*.c` (declarations at block top, spaced `func( arg )` calls); commit style `feat(fsn):`/`docs:` with why-bodies.
- The committed diff touches ONLY `src/fsn-style.h`, `src/geometry-fsn-draw.c` (Task 1) and `docs/PORTING.md`, `TODO.md` (Task 2). The `FSV_TEST_SELECT` harness in `src/sdl/main.cpp` is throwaway: MUST be fully removed before any commit (verify with `git status` / `git diff src/sdl/main.cpp` — clean).
- The cone is part of `fsn_draw_spotlight()` — it must inherit all its guards for free (select pass, overview pass, mode/node/layout/visibility checks) by being called inside it, after the rings.
- Screenshots go to `<plan workspace>/shots/` (the SDD workspace dir printed by `scripts/sdd-workspace`); name them `before-file.png`, `after-file.png`, `after-dir.png`, plus `tune-*.png` iterations.

---

### Task 1: Implement and visually tune the spotlight cone

**Files:**
- Modify: `src/fsn-style.h` (new `FSN_SPOTLIGHT_CONE_*` constants after the existing spotlight section, ~line 450)
- Modify: `src/geometry-fsn-draw.c` (new `fsn_gldraw_spotlight_cone()` helper next to `fsn_gldraw_spotlight_ring()`; one call added at the end of `fsn_draw_spotlight()`)
- Temporary (never committed): `src/sdl/main.cpp` `--screenshot` path (`FSV_TEST_SELECT` block)

**Interfaces:**
- Consumes: `FsvVertex`, `gpu_set_color()`, `gpu_draw(FSV_TRIANGLE_STRIP, …)` (`src/gpu.h:59-65`), `FSN_SPOTLIGHT_SEGMENTS`/`FSN_SPOTLIGHT_LIFT`, `fsn_draw_spotlight()`'s already-computed `cx`, `cz`, `base_z`, `rx`, `rz`, `ped`.
- Produces: the cone rendering; final tuned `FSN_SPOTLIGHT_CONE_*` values (Task 2's PORTING.md text cites the mechanism, not the numbers, so tuning freely is fine).

- [ ] **Step 1: Capture the BEFORE screenshot**

Build current HEAD, then with a temporary `FSV_TEST_SELECT` block (Step 3's code — add it now, it stays local the whole task) run:
```bash
export PATH="/opt/homebrew/bin:$PATH" && ninja -C builddir
FSV_TEST_SELECT="sdl/input.cpp" builddir/src/sdl/fsv --fsn --screenshot <workspace>/shots/before-file.png src
```
Expected: the QA report's shot — selected file box with a barely-visible pale rim. Read the PNG to confirm it reproduces the complaint.

- [ ] **Step 2: Add the constants**

In `src/fsn-style.h`, after `FSN_SPOTLIGHT_LIFT` (~line 450):

```c
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
#define FSN_SPOTLIGHT_CONE_APEX_FRAC   0.25  /* apex ellipse : base ellipse */
```

- [ ] **Step 3: Add the cone draw**

In `src/geometry-fsn-draw.c`, directly below `fsn_gldraw_spotlight_ring()`:

```c
/* One truncated translucent cone: apex ellipse (APEX_FRAC of the base)
 * at apex_z, base ellipse (the spotlight pool's own rx/ry) at base_z,
 * as a single triangle strip around the perimeter. No caps: the pool
 * rings already paint the base, and the apex is open sky. Winding puts
 * the outward faces front (the pipeline back-face culls, gpu.cpp), so
 * the viewer sees exactly one translucent layer -- the near side of the
 * beam. */
static void
fsn_gldraw_spotlight_cone( double cx, double cy, double base_z,
    double apex_z, double rx, double ry )
{
	FsvVertex verts[2 * (FSN_SPOTLIGHT_SEGMENTS + 1)];
	int i, n = 0;

	for (i = 0; i <= FSN_SPOTLIGHT_SEGMENTS; i++) {
		double theta = 2.0 * G_PI * (double)i / (double)FSN_SPOTLIGHT_SEGMENTS;
		double c = cos( theta ), s = sin( theta );

		verts[n].pos[0] = (float)(cx + FSN_SPOTLIGHT_CONE_APEX_FRAC * rx * c);
		verts[n].pos[1] = (float)(cy + FSN_SPOTLIGHT_CONE_APEX_FRAC * ry * s);
		verts[n].pos[2] = (float)apex_z;
		verts[n].normal[0] = (float)c;
		verts[n].normal[1] = (float)s;
		verts[n].normal[2] = 0.0f;
		++n;
		verts[n].pos[0] = (float)(cx + rx * c);
		verts[n].pos[1] = (float)(cy + ry * s);
		verts[n].pos[2] = (float)base_z;
		verts[n].normal[0] = (float)c;
		verts[n].normal[1] = (float)s;
		verts[n].normal[2] = 0.0f;
		++n;
	}

	gpu_set_color( 1.0f, 1.0f, 1.0f, FSN_SPOTLIGHT_CONE_ALPHA );
	gpu_draw( FSV_TRIANGLE_STRIP, verts, n, NULL, 0 );
}
```

In `fsn_draw_spotlight()`: add `double apex_z;` to the declarations, and after the ring loop (before `gpu_set_depth_test( FSV_DEPTH_LESS );`):

```c
	/* The beam above the pool -- see FSN_SPOTLIGHT_CONE_ALPHA's comment
	 * (fsn-style.h) for why the pool alone was not enough. Height rides
	 * the node's own height with a floor, so a tall pedestal's beam
	 * still clears it and a flat file box's beam is not a needle. */
	apex_z = base_z + MAX(FSN_SPOTLIGHT_CONE_MIN_HEIGHT,
	    FSN_SPOTLIGHT_CONE_HEIGHT_MULT * ped->h);
	fsn_gldraw_spotlight_cone( cx, cz, base_z + FSN_SPOTLIGHT_LIFT,
	    apex_z, rx, rz );
```

- [ ] **Step 4: The throwaway FSV_TEST_SELECT harness**

In `src/sdl/main.cpp`'s `--screenshot` path (after scan completes and before the capture — find where the screenshot branch settles the scene), add the env-gated block, same convention as Tasks A2/A3/B2/B3 (never committed):

```cpp
	// THROWAWAY test harness -- never commit (plan 2026-08-14-fsn-spotlight-cone).
	if (const char *sel = SDL_getenv("FSV_TEST_SELECT")) {
		GNode *n = node_named(sel);
		if (n != nullptr) {
			globals.current_node = n;
			camera_look_at(n);
		}
	}
```

(`node_named()` is in `src/common.c`; if the relative form doesn't resolve, use `node_from_absname()` with the absolute path instead — both exist. If the screenshot path needs extra animation ticks for the look-at morphs to settle before capture, drive `fsv_animation_tick()` in a loop the way the existing screenshot code settles the initial view — read that code and match it.)

- [ ] **Step 5: Tune against screenshots until legible**

Build and capture, reading each PNG:
```bash
export PATH="/opt/homebrew/bin:$PATH" && ninja -C builddir
FSV_TEST_SELECT="sdl/input.cpp" builddir/src/sdl/fsv --fsn --screenshot <workspace>/shots/after-file.png src
FSV_TEST_SELECT="sdl" builddir/src/sdl/fsv --fsn --screenshot <workspace>/shots/after-dir.png src
```
Acceptance criteria, judged by reading the PNGs:
1. The beam is unmistakably visible in `after-file.png` at the establishing distance (a pale translucent cone standing over the selected box, silhouetted against sky/landscape).
2. Geometry behind the beam (boxes, pedestal edges, labels) remains readable through it — if the beam is a solid white wall, halve `FSN_SPOTLIGHT_CONE_ALPHA`.
3. The directory case (`after-dir.png`) shows the beam standing over the pedestal, apex clearing its top.
4. If the cone is entirely INVISIBLE in the captures, suspect winding (back-face culling ate the near half): swap the two vertices of each strip pair (base first, then apex) and re-check.
Iterate constants (`ALPHA`, `MIN_HEIGHT`, `HEIGHT_MULT`, `APEX_FRAC`) saving `tune-N.png` shots until 1–3 hold. Keep the final `after-*.png` pair in the workspace — the controller sends them to the user.

- [ ] **Step 6: Remove the harness, verify clean, run the suite**

Delete the `FSV_TEST_SELECT` block. `git diff src/sdl/main.cpp` must be empty. Then:
`export PATH="/opt/homebrew/bin:$PATH" && ninja -C builddir && meson test -C builddir`
Expected: all 6 tests PASS (the cone is draw-path only; no libfsvcore change, so nothing headless can regress — the suite run is the no-breakage check, not the feature check).

- [ ] **Step 7: Commit**

```bash
git add src/fsn-style.h src/geometry-fsn-draw.c
git commit -m "feat(fsn): draw the selection spotlight's light cone, not just its pool

User QA on the framing fix: the Task B3 ground pool reads as a faint
rim -- white at ~0.55 composited alpha on a light-grey pedestal is
structurally low-contrast, and in a packed box row the ellipse hides
under the very boxes it surrounds. US5861885's spotlight is a beam
from above, not only the pool it casts: add a truncated translucent
cone from the node-height-scaled apex down to the existing pool
ellipse, drawn by fsn_draw_spotlight() itself so every guard (select
pass, overview pass, visibility) is inherited. Back-face culling shows
the beam's near half only -- one translucent layer. Constants tuned
against --screenshot captures of this repo's own src/ tree (file and
directory selections), same throwaway FSV_TEST_SELECT harness
convention as Tasks A2/A3/B2/B3, removed before this commit."
```

---

### Task 2: Docs sync and push

**Files:**
- Modify: `docs/PORTING.md` (Task B3 section: the spotlight write-up gains a cone follow-up note; grep `fsn_draw_spotlight` / `light pool` for the section)
- Modify: `TODO.md` ("UX rough edges" → the "Selection spotlight is not deployment-aware (Task B3)" bullet: the caveat now covers the cone too)

**Interfaces:**
- Consumes: Task 1's commit hash and the mechanism description (cone above pool, same guards).
- Produces: docs only.

- [ ] **Step 1: PORTING.md follow-up note**

In the Task B3 section, at the end of the spotlight implementation write-up (near where the ring table/decal design is described — grep `FSN_SPOTLIGHT`), append a short follow-up paragraph in the doc's voice:

```markdown
**Follow-up (post-v0.3, user QA on the framing fix):** the pool alone
proved nearly invisible in real use — white at ~0.55 composited alpha
on a light-grey pedestal top, mostly self-occluded inside a packed box
row. `fsn_draw_spotlight()` now also draws US5861885's actual beam: a
truncated translucent cone (`FSN_SPOTLIGHT_CONE_*`, fsn-style.h) from a
node-height-scaled apex down to the pool ellipse, silhouetting against
sky and pedestal faces instead of the surface it sits on. Same guards
by construction (drawn by the same function); same deployment-caveat as
the pool (below). Tuned via `--screenshot` captures, throwaway
`FSV_TEST_SELECT` harness, removed before commit.
```

- [ ] **Step 2: TODO.md deployment-caveat bullet**

In "UX rough edges", extend the existing spotlight bullet's first sentence:

Replace `**Selection spotlight is not deployment-aware (Task B3):**` with `**Selection spotlight is not deployment-aware (Task B3; applies to the light cone too):**` — rest of the bullet unchanged.

- [ ] **Step 3: Commit and push**

```bash
git add docs/PORTING.md TODO.md docs/superpowers/plans/2026-08-14-fsn-spotlight-cone.md
git commit -m "docs: record the spotlight light-cone follow-up

The pool-only decal was a QA miss in real use; PORTING.md's B3 section
now records the cone and its shared guards/caveats, and TODO.md's
deployment-awareness rough edge explicitly covers the cone."
git push origin fsn-mode
```

Watch CI to green (`gh run list -R w3cdotorg/fsv --branch fsn-mode --limit 1`, `gh run watch <id> -R w3cdotorg/fsv --exit-status`). If CI fails, report — do not attempt fixes.

---

## Notes for the final review

- The feature is visual-only; the empirical evidence is the `after-*.png` pair in the plan workspace — the reviewer should read them, not just the diff.
- The plan file itself must be committed before merge (lesson from the previous plan's final review).
