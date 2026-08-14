# MapV Squarify + Scan Exclusion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make MapV usable on real checkouts: squarified treemap layout, √size default area scale (linear/log options), and built-in scan exclusion of VCS/build directories.

**Architecture:** Three cooperating changes, each independently shippable. (1) A pure `src/squarify.c` module (Bruls-2000 squarified treemap; in `libfsvcore`, unit-tested headlessly) replaces `mapv_init_recursive()`'s greedy full-width-row passes in `src/geometry.c`. (2) A per-file area-weight mapping (√ default, linear/log options) with per-directory weight sums computed in a recursive pre-pass into a new `MapVGeomParams::area_weight` field (`common.h`'s `geomparams[5]` grows to `[6]` to make room), selectable from a new `Display → MapV area scale` submenu, persisted via nvstore, relayout via `geometry_init( FSV_MAPV )`. (3) `src/scanfs.c` gains a built-in, conservative excluded-directory-name list (on by default, toggleable): matching directories are unlinked/destroyed right after `stat_node()` (same path as stat failures) so they are never traversed — plus a `Vis → Skip VCS/build dirs` checkbox persisted via nvstore that triggers `app_request_rescan()`. Verification: two new headless meson tests (`squarify`, `scanfs_exclude`) plus `--mapv --screenshot` before/after captures of this repo (the 2026-08-09 "sliver" diagnosis is the acceptance demo).

**Tech Stack:** C89 GLib C (core), C++ SDL/ImGui (frontend menus), Meson/ninja, nvstore persistence, established screenshot verification.

**Spec:** `docs/superpowers/specs/2026-08-14-mapv-squarify-scan-exclude-design.md` (committed, `ea40a5f`) — the binding authority; it records the user's four design choices.

## Global Constraints

- Build dir `builddir`; `export PATH="/opt/homebrew/bin:$PATH"` first; repo root `/Users/willow/Sites/_Claude_output/fsn/fsv`; branch `fsn-mode`. `-DDEBUG` live in tests.
- C dialect: C89-flavored GLib C in `src/*.c`/`tests/` (declarations at block top, `boolean`/`TRUE`/`FALSE`, spaced `func( arg )` in `src/*.c`); `src/squarify.c` stays PURE (stdlib only — no glib, no gpu, no globals) so it belongs in `libfsvcore` unconditionally.
- Commit per task, `feat:`/`fix:`/`test:`/`docs:` style with why-bodies. ≤5 files per task.
- Exclusion list (exact basename match, directories only, V1): `.git` `.svn` `.hg` `node_modules` `__pycache__` `.venv` `.cache` `builddir` `.builddir`. Exclusion default: ENABLED. Area scale default: `MAPV_SCALE_SQRT`.
- nvstore persistence pattern to copy: `src/sdl/ui_dialogs.cpp:658-664` (write) and `:834-838` (read with default), key style `key_open_files_allowed`.
- Screenshots go to `<plan workspace>/shots/` with descriptive names; `--mapv --screenshot` needs no selection harness.
- GTK frontend: core changes (squarify, weights default √, exclusion default ON) apply there by default with NO GTK UI added — the spec accepts this; the GTK arm must still BUILD (CI covers it).

---

### Task 1: Pure squarify module, TDD

**Files:**
- Create: `src/squarify.h`, `src/squarify.c`
- Create: `tests/test_squarify.c`
- Modify: `src/meson.build` (add `'squarify.c'` to `fsvcore_src`), `tests/meson.build` (new entry using `core_test_deps`; this test needs no fixture and no stubs — it links only the pure module? No: linking libfsvcore objects pulls symbols needing stubs; use the same recipe as the others, `headless_stubs_src` included, which is proven to link)

**Interfaces:**
- Produces: `SquarifyRect { double x, y, w, h; }` and `void squarify_layout( const SquarifyRect *bounds, const double *areas, int n, SquarifyRect *out_rects );` — Task 2 consumes exactly this. Semantics: areas are RELATIVE (function normalizes to tile `bounds` exactly); `out_rects[i]` corresponds to `areas[i]` (input order preserved); internal descending-area sort per the algorithm.

- [ ] **Step 1: Write the header**

`src/squarify.h`:

```c
/* squarify.h — SPDX-License-Identifier: MIT
 *
 * Pure squarified-treemap layout (Bruls, Huizing, van Wijk 2000).
 * No glib, no gpu, no globals: this is libfsvcore's only fully
 * dependency-free module, which is what makes MapV's layout unit-
 * testable headlessly (geometry.c itself is GL-bound). */

#ifdef FSV_SQUARIFY_H
	#error
#endif
#define FSV_SQUARIFY_H

typedef struct {
	double x, y; /* origin corner */
	double w, h; /* extents, both > 0 */
} SquarifyRect;

/* Lays out n blocks with the given RELATIVE areas inside `bounds`:
 * the function normalizes internally so the rects tile bounds exactly.
 * out_rects (caller-allocated, n entries) is written in the INPUT's
 * order — internally the algorithm sorts descending by area (its
 * aspect-ratio guarantee needs that), but callers keep their own
 * node <-> rect correspondence by index. Zero/negative areas are
 * treated as a tiny epsilon share so every block gets a real rect. */
void squarify_layout( const SquarifyRect *bounds, const double *areas,
                      int n, SquarifyRect *out_rects );
```

- [ ] **Step 2: Write the failing test**

`tests/test_squarify.c` — four invariants plus the improvement demonstration:

```c
/* tests/test_squarify.c
 *
 * SPDX-License-Identifier: MIT
 *
 * Invariants of the pure squarified-treemap module (src/squarify.c),
 * the replacement for geometry.c's 1999 greedy full-width-row MapV
 * layout ("some nodes too wide, some too tall, frontmost rows
 * paper-thin" -- the upstream TODO's own words):
 *
 *   1. exact tiling: rect areas sum to bounds' area;
 *   2. containment: every rect inside bounds (within TOL);
 *   3. no overlaps: pairwise intersection area is zero (within TOL);
 *   4. input-order correspondence: biggest input area gets the
 *      biggest rect, and each out_rects[i] area is proportional to
 *      areas[i];
 *   5. the point of it all: on a byte-skewed distribution (one huge
 *      block + many small ones -- the .git case), the worst aspect
 *      ratio beats the old algorithm's full-width-rows result,
 *      recomputed here as the reference. */

#include <assert.h>
#include <math.h>

#include "squarify.h"

#define TOL 1.0e-6
#define N_SKEW 65

static double
rect_area(const SquarifyRect *r)
{
	return r->w * r->h;
}

static double
overlap_1d(double a0, double a1, double b0, double b1)
{
	double lo = a0 > b0 ? a0 : b0;
	double hi = a1 < b1 ? a1 : b1;
	return hi > lo ? hi - lo : 0.0;
}

static double
worst_aspect(const SquarifyRect *rects, int n)
{
	double worst = 1.0;
	int i;

	for (i = 0; i < n; i++) {
		double r = rects[i].w / rects[i].h;
		if (r < 1.0)
			r = 1.0 / r;
		if (r > worst)
			worst = r;
	}
	return worst;
}

/* The OLD algorithm, reduced to its aspect-ratio-relevant core (from
 * geometry.c's pass 2/4 before this change): greedy rows spanning the
 * FULL bounds width; a row closes when its newest block's aspect
 * (width/depth) drops below 1. Returns the worst aspect ratio it
 * would produce for the given areas. */
static double
old_rows_worst_aspect(const SquarifyRect *bounds, const double *areas,
    int n)
{
	double total = 0.0, scale;
	double row_area = 0.0, worst = 1.0;
	double bw = bounds->w;
	int i, row_start = 0;

	for (i = 0; i < n; i++)
		total += areas[i];
	scale = (bounds->w * bounds->h) / total;

	for (i = 0; i < n; i++) {
		double a = areas[i] * scale;
		double depth, width, r;
		int j;

		row_area += a;
		depth = row_area / bw;
		width = a / depth;
		if (width / depth < 1.0 || i == n - 1) {
			/* Row closes: score every block in it */
			for (j = row_start; j <= i; j++) {
				width = areas[j] * scale / depth;
				r = width / depth;
				if (r < 1.0)
					r = 1.0 / r;
				if (r > worst)
					worst = r;
			}
			row_area = 0.0;
			row_start = i + 1;
		}
	}
	return worst;
}

int
main(void)
{
	SquarifyRect bounds;
	SquarifyRect rects[N_SKEW];
	double areas[N_SKEW];
	double sum, worst_new, worst_old;
	int i, j;

	bounds.x = -320.0;
	bounds.y = 100.0;
	bounds.w = 1200.0;
	bounds.h = 1000.0;

	/* The .git case: one block holding ~94% of the total, 64 small
	 * ones sharing the rest (with some variety) */
	areas[0] = 100000.0;
	for (i = 1; i < N_SKEW; i++)
		areas[i] = 50.0 + 7.0 * (double)(i % 9);

	squarify_layout(&bounds, areas, N_SKEW, rects);

	/* 1. exact tiling */
	sum = 0.0;
	for (i = 0; i < N_SKEW; i++)
		sum += rect_area(&rects[i]);
	assert(fabs(sum - bounds.w * bounds.h) < TOL * bounds.w * bounds.h);

	/* 2. containment */
	for (i = 0; i < N_SKEW; i++) {
		assert(rects[i].w > 0.0 && rects[i].h > 0.0);
		assert(rects[i].x >= bounds.x - TOL);
		assert(rects[i].y >= bounds.y - TOL);
		assert(rects[i].x + rects[i].w <= bounds.x + bounds.w + TOL);
		assert(rects[i].y + rects[i].h <= bounds.y + bounds.h + TOL);
	}

	/* 3. no overlaps */
	for (i = 0; i < N_SKEW; i++)
		for (j = i + 1; j < N_SKEW; j++) {
			double ox = overlap_1d(rects[i].x, rects[i].x + rects[i].w,
			    rects[j].x, rects[j].x + rects[j].w);
			double oy = overlap_1d(rects[i].y, rects[i].y + rects[i].h,
			    rects[j].y, rects[j].y + rects[j].h);
			assert(ox * oy < TOL * bounds.w * bounds.h);
		}

	/* 4. order correspondence: out_rects[i] area proportional to
	 * areas[i] (same normalization for all), so the huge input is the
	 * huge rect */
	{
		double unit = rect_area(&rects[1]) / areas[1];

		for (i = 0; i < N_SKEW; i++)
			assert(fabs(rect_area(&rects[i]) - areas[i] * unit)
			    < 1.0e-3 * areas[0] * unit);
		assert(rect_area(&rects[0]) > rect_area(&rects[1]));
	}

	/* 5. the improvement that motivates the rewrite */
	worst_new = worst_aspect(rects, N_SKEW);
	worst_old = old_rows_worst_aspect(&bounds, areas, N_SKEW);
	assert(worst_new < worst_old);
	/* And not merely "less bad": squarify's blocks stay recognizably
	 * block-like even here */
	assert(worst_new < 8.0);

	/* Degenerate input: all-zero areas must still tile (equal split),
	 * not crash or divide by zero */
	{
		double zeros[4] = { 0.0, 0.0, 0.0, 0.0 };
		SquarifyRect zrects[4];

		squarify_layout(&bounds, zeros, 4, zrects);
		sum = 0.0;
		for (i = 0; i < 4; i++)
			sum += rect_area(&zrects[i]);
		assert(fabs(sum - bounds.w * bounds.h) < TOL * bounds.w * bounds.h);
	}

	return 0;
}
```

Meson entry (`tests/meson.build`, same recipe as the others):

```meson
# Pure squarified-treemap module (src/squarify.c) -- MapV's layout
# algorithm, extracted precisely so it can be tested here without the
# GL-bound geometry.c around it.
test_squarify = executable('test_squarify',
  ['test_squarify.c', headless_stubs_src],
  objects: libfsvcore.extract_all_objects(recursive: false),
  dependencies: core_test_deps,
  include_directories: incdir)
test('squarify', test_squarify)
```

And `src/meson.build`: `fsvcore_src` gains `'squarify.c'`.

- [ ] **Step 3: Run — must FAIL to link (module absent)**

`export PATH="/opt/homebrew/bin:$PATH" && ninja -C builddir` → link error on `squarify_layout`. Record it.

- [ ] **Step 4: Implement the module**

`src/squarify.c`:

```c
/* squarify.c — SPDX-License-Identifier: MIT
 * See squarify.h. Pure stdlib on purpose. */

#include <math.h>
#include <stdlib.h>

#include "squarify.h"

typedef struct {
	double area;
	int index; /* position in the caller's input */
} SqBlock;

/* Descending by area; ties broken by input order for determinism */
static int
sqblock_cmp( const void *va, const void *vb )
{
	const SqBlock *a = (const SqBlock *)va;
	const SqBlock *b = (const SqBlock *)vb;

	if (a->area > b->area)
		return -1;
	if (a->area < b->area)
		return 1;
	return a->index - b->index;
}

/* Worst aspect ratio of blocks [first..last] laid as one run of
 * thickness (run_area / side) along a side of length `side` */
static double
run_worst_aspect( const SqBlock *blocks, int first, int last,
    double side, double run_area )
{
	double thickness = run_area / side;
	double worst = 1.0;
	int i;

	for (i = first; i <= last; i++) {
		double len = blocks[i].area / thickness;
		double r = (len > thickness) ? (len / thickness)
		                             : (thickness / len);
		if (r > worst)
			worst = r;
	}
	return worst;
}

void
squarify_layout( const SquarifyRect *bounds, const double *areas, int n,
    SquarifyRect *out_rects )
{
	SqBlock *blocks;
	SquarifyRect free_rect;
	double total = 0.0, scale;
	int i, first;

	if (n <= 0)
		return;

	blocks = (SqBlock *)calloc( (size_t)n, sizeof(SqBlock) );
	for (i = 0; i < n; i++) {
		blocks[i].area = (areas[i] > 0.0) ? areas[i] : 0.0;
		blocks[i].index = i;
		total += blocks[i].area;
	}
	if (total <= 0.0) {
		/* Degenerate: nothing has area. Give every block an equal
		 * share rather than dividing by zero. */
		for (i = 0; i < n; i++)
			blocks[i].area = 1.0;
		total = (double)n;
	} else {
		/* A zero-area block among real ones still deserves a sliver
		 * (it must remain pickable): the smallest positive share */
		double epsilon = total * 1.0e-9;

		for (i = 0; i < n; i++)
			if (blocks[i].area <= 0.0) {
				blocks[i].area = epsilon;
				total += epsilon;
			}
	}

	qsort( blocks, (size_t)n, sizeof(SqBlock), sqblock_cmp );

	/* Normalize: block areas tile bounds exactly */
	scale = (bounds->w * bounds->h) / total;
	for (i = 0; i < n; i++)
		blocks[i].area *= scale;

	free_rect = *bounds;
	first = 0;
	while (first < n) {
		double side = (free_rect.w < free_rect.h) ? free_rect.w
		                                          : free_rect.h;
		double run_area = blocks[first].area;
		double worst = run_worst_aspect( blocks, first, first, side,
		    run_area );
		double thickness, along;
		int last = first;

		/* Grow the run while the worst aspect ratio improves */
		while (last + 1 < n) {
			double try_area = run_area + blocks[last + 1].area;
			double try_worst = run_worst_aspect( blocks, first,
			    last + 1, side, try_area );

			if (try_worst > worst)
				break;
			last++;
			run_area = try_area;
			worst = try_worst;
		}

		/* The final run must absorb the whole remaining free rect
		 * (float dust would otherwise leave a hairline strip) */
		if (last == n - 1)
			thickness = (free_rect.w < free_rect.h)
			    ? free_rect.h : free_rect.w;
		else
			thickness = run_area / side;

		/* Emit the run as one strip along the shorter side */
		along = 0.0;
		if (free_rect.w >= free_rect.h) {
			/* Wider than tall: strip on the left edge, blocks
			 * stacked in y */
			if (last == n - 1)
				thickness = free_rect.w;
			for (i = first; i <= last; i++) {
				double len = blocks[i].area / thickness;
				SquarifyRect *r = &out_rects[blocks[i].index];

				r->x = free_rect.x;
				r->y = free_rect.y + along;
				r->w = thickness;
				r->h = len;
				along += len;
			}
			/* Final strip: stretch the last block to the edge */
			if (out_rects[blocks[last].index].y
			    + out_rects[blocks[last].index].h
			    < free_rect.y + free_rect.h)
				out_rects[blocks[last].index].h =
				    free_rect.y + free_rect.h
				    - out_rects[blocks[last].index].y;
			free_rect.x += thickness;
			free_rect.w -= thickness;
		} else {
			/* Taller than wide: strip on the bottom edge, blocks
			 * in x */
			if (last == n - 1)
				thickness = free_rect.h;
			for (i = first; i <= last; i++) {
				double len = blocks[i].area / thickness;
				SquarifyRect *r = &out_rects[blocks[i].index];

				r->x = free_rect.x + along;
				r->y = free_rect.y;
				r->w = len;
				r->h = thickness;
				along += len;
			}
			if (out_rects[blocks[last].index].x
			    + out_rects[blocks[last].index].w
			    < free_rect.x + free_rect.w)
				out_rects[blocks[last].index].w =
				    free_rect.x + free_rect.w
				    - out_rects[blocks[last].index].x;
			free_rect.y += thickness;
			free_rect.h -= thickness;
		}
		first = last + 1;
	}

	free( blocks );
}
```

NOTE to the implementer: the float-dust handling above (final-run stretch) is a starting point — if invariant 1 or 3 fails at TOL in the test, tighten it (e.g. recompute the last strip's thickness from the remaining free rect rather than run_area) instead of loosening TOL. Exact tiling is the contract.

- [ ] **Step 5: Run the test — PASS — then the whole suite**

`meson test -C builddir squarify --print-errorlogs && meson test -C builddir` → 7/7.

- [ ] **Step 6: Commit**

```bash
git add src/squarify.c src/squarify.h src/meson.build tests/test_squarify.c tests/meson.build
git commit -m "feat: add a pure squarified-treemap module (Bruls 2000)

MapV's 1999 layout is greedy full-width rows with an empty
'optimization' pass; the fix is the squarified treemap, extracted as
libfsvcore's only dependency-free module precisely so it is headless-
testable (geometry.c is GL-bound). Tested for exact tiling,
containment, non-overlap, input-order correspondence, and a worst-
aspect-ratio win over the old row algorithm (recomputed in the test as
the reference) on the byte-skewed .git-style distribution that
motivates the rewrite."
```

---

### Task 2: geometry.c integration (layout only — sizes still linear)

**Files:**
- Modify: `src/geometry.c:598-793` (`mapv_init_recursive()`: keep pass 1, replace passes 2–4 with a `squarify_layout()` call; delete `struct MapVRow` and the row machinery)

**Interfaces:**
- Consumes: Task 1's `squarify_layout()` / `SquarifyRect` (include `"squarify.h"`).
- Produces: same `MAPV_GEOM_PARAMS` per-node contract as before (c0/c1/height, recursion into dirs) — nothing downstream changes.

- [ ] **Step 1: Capture BEFORE screenshots**

At current HEAD (old layout still in): build, then
`builddir/src/sdl/fsv --mapv --screenshot <workspace>/shots/before-mapv-repo.png .`
run from the repo root (scans the checkout itself, `.git`/`builddir` included since exclusion doesn't exist yet). READ the PNG — it should show the diagnosed pathology (dominant plane + sliver).

- [ ] **Step 2: Replace the row machinery**

In `mapv_init_recursive()`: keep everything through pass 1 (block list construction, `total_block_area`, `scale_factor` becomes unnecessary — squarify normalizes internally, but KEEP computing per-block `area` (size + border margin) since the border-exactness math in the emit loop needs `scale_factor * size`; simplest faithful shape:

1. Build parallel arrays from `block_list`: `double *areas` and `GNode **nodes` (`xmalloc`; n = child count).
2. Bounds: `bounds.x = MAPV_NODE_CENTER_X(dnode) - 0.5 * dir_dims.x; bounds.y = MAPV_NODE_CENTER_Y(dnode) - 0.5 * dir_dims.y; bounds.w = dir_dims.x; bounds.h = dir_dims.y;`
3. `squarify_layout( &bounds, areas, n, rects );`
4. Per node i: `block_dims.x = rects[i].w; block_dims.y = rects[i].h;` then reuse the EXISTING border math verbatim (`size` recomputation, `area = scale_factor * size`, the quadratic `border` formula — where `block->area` is `areas[i] * (dir_area / total_block_area)`, i.e. keep `scale_factor` for this) and assign:
   `gparams->c0.x = rects[i].x + border; gparams->c0.y = rects[i].y + border; gparams->c1.x = rects[i].x + rects[i].w - border; gparams->c1.y = rects[i].y + rects[i].h - border;`
5. Heights + recursion exactly as before (`mapv_dir_height`/`mapv_leaf_height`, recurse into dirs).
6. Free the arrays and the block list; `struct MapVRow`, `row_list`, `next_first_block`, `start_pos`, `pos` all go away.

Guard: the border formula's discriminant (`SQR(k) - 4.0 * (block->area - area)`) can go slightly negative on near-degenerate slivers; clamp to 0 before `sqrt()` (pre-existing risk made more reachable by extreme aspect inputs — one `MAX(0.0, …)`).

- [ ] **Step 3: Build, suite, AFTER screenshot**

`ninja -C builddir && meson test -C builddir` → 7/7 (nothing headless reads MapV layout).
`builddir/src/sdl/fsv --mapv --screenshot <workspace>/shots/after-squarify-repo.png .` — READ it: blocks near-square at every level, no full-width paper-thin rows. The huge `.git` block still dominates (exclusion and √ scale come later) — that's expected here.

- [ ] **Step 4: Commit**

```bash
git add src/geometry.c
git commit -m "feat(mapv): replace the greedy row layout with the squarify module

mapv_init_recursive()'s passes 2-4 (full-width rows, closed on
aspect<1, with the 'optimization' pass an empty note-to-self since
1999) become one squarify_layout() call; pass 1's block/border
accounting and the exact quadratic border inset are kept verbatim.
Same MAPV_GEOM_PARAMS contract out, so drawing, picking, camera and
labels are untouched. Verified by before/after --mapv screenshots on
this repo and the 7/7 suite."
```

---

### Task 3: Area weights (√ default) + Display menu

**Files:**
- Modify: `src/common.h:219` (`geomparams[5]` → `geomparams[6]`, comment why)
- Modify: `src/geometry.h` (add `double area_weight;` to `MapVGeomParams`; add `typedef enum { MAPV_SCALE_SQRT, MAPV_SCALE_LINEAR, MAPV_SCALE_LOG } MapVAreaScale;` and `void mapv_set_area_scale( MapVAreaScale scale ); MapVAreaScale mapv_get_area_scale( void );`)
- Modify: `src/geometry.c` (weight pre-pass + all size→weight switches + the scale accessors)
- Modify: `src/sdl/ui_main.cpp` (Display submenu + nvstore read/write)

**Interfaces:**
- Consumes: Task 2's integrated layout.
- Produces: `mapv_set_area_scale()` / `mapv_get_area_scale()` (C linkage — geometry.h is already consumed by the C++ frontend); nvstore key `key_mapv_area_scale` (int, default `MAPV_SCALE_SQRT`).

- [ ] **Step 1: The weight machinery in geometry.c**

- `static MapVAreaScale mapv_area_scale = MAPV_SCALE_SQRT;` + accessors (setter does NOT relayout itself; the caller decides — keeps it pure state).
- Mapping helper:

```c
/* Area weight of a byte size under the current scale. The MAX(256,
 * size) floor predates this (it kept tiny files visible under the
 * linear scale) and applies to the INPUT, so its intent survives all
 * three scales. */
static double
mapv_map_size( int64 size )
{
	double s = (double)MAX(256, size);

	switch (mapv_area_scale) {
		case MAPV_SCALE_SQRT:
		return sqrt( s );

		case MAPV_SCALE_LINEAR:
		return s;

		case MAPV_SCALE_LOG:
		return log2( 1.0 + s );

		SWITCH_FAIL
	}
	return s;
}

/* Recursive pre-pass: every node's area weight, files mapped
 * directly, directories = own entry's mapped size + sum of children
 * (mapping the SUM instead would break parent/child proportions) */
static double
mapv_weigh_recursive( GNode *node )
{
	double w = mapv_map_size( NODE_DESC(node)->size );
	GNode *child;

	if (NODE_IS_DIR(node))
		for (child = node->children; child != NULL; child = child->next)
			w += mapv_weigh_recursive( child );
	MAPV_GEOM_PARAMS(node)->area_weight = w;
	return w;
}
```

- `mapv_init()`: call `mapv_weigh_recursive( root_dnode );` FIRST; root dims switch from `subtree.size` to `MAPV_GEOM_PARAMS(root_dnode)->area_weight` (metanode weight handling: weigh from root_dnode, and use the root's weight — the metanode's own entry isn't drawn).
- `mapv_init_recursive()` pass 1 + border math: everywhere `size` / `size + subtree.size` feeds area (`k = sqrt(size) + border`, `area = scale_factor * size`), use `MAPV_GEOM_PARAMS(node)->area_weight` instead (weight is already area-dimensioned: `k = sqrt( weight_as_area )`? NO — careful, keep the existing shape: the old code treats `size` AS the area and takes `sqrt(size)` for the side. The weight replaces `size` one-for-one: `k = sqrt( MAPV_GEOM_PARAMS(node)->area_weight ) + nominal_border; area = SQR(k);` and in the border-exactness step `area = scale_factor * MAPV_GEOM_PARAMS(node)->area_weight;`).

- [ ] **Step 2: geomparams room + struct field**

`src/common.h:219`: `double geomparams[6];` with the comment extended: `/* Geometry parameters (6: MapV grew area_weight in the 2026 squarify rework) */`. `src/geometry.h`: `area_weight` field appended to `struct _MapVGeomParams` (after `height`), comment `/* Scaled area weight (mapv_map_size sums), computed by the layout pre-pass */`.

- [ ] **Step 3: Display submenu + persistence (ui_main.cpp)**

In the `Display` menu block (after the Landscape submenu, `src/sdl/ui_main.cpp:331-…`):

```cpp
if (ImGui::BeginMenu("MapV area scale")) {
	// Radio semantics via checkmarks, same idiom as the Colors menu.
	static const struct { const char *label; int scale; } kScales[] = {
		{ "Square root", MAPV_SCALE_SQRT },
		{ "Linear (bytes)", MAPV_SCALE_LINEAR },
		{ "Logarithmic", MAPV_SCALE_LOG },
	};
	for (const auto &s : kScales) {
		if (ImGui::MenuItem(s.label, nullptr,
		    mapv_get_area_scale() == s.scale)) {
			mapv_set_area_scale((MapVAreaScale)s.scale);
			save_mapv_area_scale();      // nvstore, below
			geometry_init(FSV_MAPV);     // relayout, not rescan
			globals.need_redraw = TRUE;
		}
	}
	ImGui::EndMenu();
}
```

Plus `save_mapv_area_scale()` / startup load, copying the `open_files_allowed` nvstore pattern verbatim (key `key_mapv_area_scale`, `nvs_read_int_default`-style API — check nvstore.h for the exact int read/write names and use them; if only boolean/string exist, store the int as string via the existing API). Startup load happens where `open_files_allowed` loads. NOTE: `geometry_init(FSV_MAPV)` while the current mode is MapV rebuilds the layout; guard the call with `if (globals.fsv_mode == FSV_MAPV)` and otherwise let the next mode switch pick the new scale up naturally (geometry_init runs on every mode entry — verify in fsv.c:118 and say so in the commit).

- [ ] **Step 4: Build, suite, capture**

`ninja -C builddir && meson test -C builddir` → 7/7. Capture `<workspace>/shots/after-sqrt-repo.png` (same command as Task 2) and READ it: with √ default the small files are now clearly visible next to the (still present) `.git` block.

- [ ] **Step 5: Commit**

```bash
git add src/common.h src/geometry.h src/geometry.c src/sdl/ui_main.cpp
git commit -m "feat(mapv): sqrt-size area scale by default, linear/log in Display menu

Area-proportional-to-bytes is what turns a checkout into one pack-file
plane with every source file in a sliver. Weights are mapped per FILE
(sqrt default; linear keeps the historical look; log2 for extreme
skew) and summed per directory in a pre-pass into a new
MapVGeomParams::area_weight (common.h's geomparams grows 5->6 to make
room) -- mapping the sum instead would break parent/child area
consistency. The MAX(256,size) visibility floor moves to the mapping
input, preserving its intent across scales. Display -> 'MapV area
scale' picks the scale, persists it via nvstore, and relayouts (no
rescan)."
```

---

### Task 4: Scan exclusion + Vis toggle, TDD

**Files:**
- Modify: `src/scanfs.h` (declare `void scanfs_set_exclusion( boolean enabled );` `boolean scanfs_get_exclusion( void );`)
- Modify: `src/scanfs.c` (the list, the accessors, the `process_dir()` hook)
- Create: `tests/test_scanfs_exclude.c`
- Modify: `tests/meson.build` (new entry, fixture-free — it builds its own temp tree)
- Modify: `src/sdl/ui_main.cpp` (Vis-menu checkbox + nvstore + `app_request_rescan()`)

**Interfaces:**
- Consumes: `stat_node()` flow in `process_dir()` (`src/scanfs.c:173-…`); `app_request_rescan()` (`src/sdl/app.h:37`).
- Produces: `scanfs_set_exclusion()` / `scanfs_get_exclusion()` (default TRUE); nvstore key `key_scan_exclusion`.

- [ ] **Step 1: Write the failing test**

`tests/test_scanfs_exclude.c`:

```c
/* tests/test_scanfs_exclude.c
 *
 * SPDX-License-Identifier: MIT
 *
 * The built-in scan exclusion (upstream-1999 TODO "a way to exclude
 * directories from the scan"; today's everyday case is .git/build
 * dirs dominating MapV's byte-proportional layout). Directories whose
 * basename is on scanfs.c's built-in list are never traversed and
 * never enter the tree; FILES with those names are never excluded;
 * the toggle restores full scanning. The tree is built here at
 * runtime (mkdtemp) rather than in tests/fixture -- a committed
 * directory literally named .git inside the fixture would fight git
 * itself.
 *
 * LINKING: same recipe as test_scanfs (scanfs.c is in libfsvcore). */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "common.h" /* pulls in glib.h, and must precede it: G_LOG_DOMAIN */
#include "fsv.h"
#include "scanfs.h"

static GNode *
child_named(GNode *parent, const char *name)
{
	GNode *node;

	for (node = parent->children; node != NULL; node = node->next)
		if (strcmp(NODE_DESC(node)->name, name) == 0)
			return node;

	return NULL;
}

static void
make_file(const char *dir, const char *name, int bytes)
{
	char path[1024];
	FILE *fp;
	int i;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	fp = fopen(path, "w");
	assert(fp != NULL);
	for (i = 0; i < bytes; i++)
		fputc('x', fp);
	fclose(fp);
}

int
main(void)
{
	char tmpl[] = "/tmp/fsv-exclude-XXXXXX";
	char sub[1024];
	char *root;
	GNode *dnode;

	root = mkdtemp(tmpl);
	assert(root != NULL);

	/* .git/ with content (the thing to skip) */
	snprintf(sub, sizeof(sub), "%s/.git", root);
	assert(mkdir(sub, 0755) == 0);
	make_file(sub, "pack.bin", 4096);
	/* node_modules/ with content */
	snprintf(sub, sizeof(sub), "%s/node_modules", root);
	assert(mkdir(sub, 0755) == 0);
	make_file(sub, "dep.js", 512);
	/* normal content: a dir and files */
	snprintf(sub, sizeof(sub), "%s/src", root);
	assert(mkdir(sub, 0755) == 0);
	make_file(sub, "main.c", 100);
	make_file(root, "README", 50);
	/* a FILE named .git -- must never be excluded (dirs only) */
	snprintf(sub, sizeof(sub), "%s/src", root);
	make_file(sub, ".git", 10);

	/* Exclusion ON (the default) */
	assert(scanfs_get_exclusion());
	scanfs(root);
	assert(globals.fstree != NULL);
	dnode = root_dnode;
	assert(dnode != NULL);
	assert(child_named(dnode, ".git") == NULL);
	assert(child_named(dnode, "node_modules") == NULL);
	assert(child_named(dnode, "README") != NULL);
	assert(child_named(dnode, "src") != NULL);
	/* the FILE named .git inside src/ survives */
	assert(child_named(child_named(dnode, "src"), ".git") != NULL);
	assert(!NODE_IS_DIR(child_named(child_named(dnode, "src"), ".git")));

	/* Exclusion OFF: everything scans */
	scanfs_set_exclusion(FALSE);
	scanfs(root);
	dnode = root_dnode;
	assert(child_named(dnode, ".git") != NULL);
	assert(NODE_IS_DIR(child_named(dnode, ".git")));
	assert(child_named(child_named(dnode, ".git"), "pack.bin") != NULL);
	assert(child_named(dnode, "node_modules") != NULL);

	return 0;
}
```

Meson entry (no FIXTURE_DIR c_args needed):

```meson
# Built-in scan exclusion (scanfs.c): excluded dir names never enter
# the tree; files with those names always do; the toggle restores full
# scanning. Builds its own mkdtemp tree (a committed .git fixture dir
# would fight git itself).
test_scanfs_exclude = executable('test_scanfs_exclude',
  ['test_scanfs_exclude.c', headless_stubs_src],
  objects: libfsvcore.extract_all_objects(recursive: false),
  dependencies: core_test_deps,
  include_directories: incdir)
test('scanfs_exclude', test_scanfs_exclude)
```

NOTE: if a second `scanfs()` call in one process trips over global state (name_strchunk, node ids), fix the TEST to match how the app rescans (find the rescan path's cleanup — `fsv_load()` in fsv.c — and call the same). If rescan-in-process genuinely can't work headlessly, split the OFF case into a second executable and say so in the report.

- [ ] **Step 2: Run — must FAIL** (`scanfs_get_exclusion` unresolved). Record.

- [ ] **Step 3: Implement in scanfs.c/h**

`src/scanfs.h` gains the two prototypes. `src/scanfs.c`:

```c
/* Built-in scan exclusion (2026 rework; the upstream-1999 TODO's "way
 * to exclude directories" -- originally slow AFS mounts, today
 * .git/build dirs that also dominate MapV's byte-proportional
 * layout). Deliberately conservative: exact basename match,
 * DIRECTORIES only, no 'build'/'target'/'dist' (too many real-content
 * false positives). The SDL frontend exposes a toggle (Vis menu);
 * default is on. */
static const char *excluded_dir_names[] = {
	".git", ".svn", ".hg", "node_modules", "__pycache__",
	".venv", ".cache", "builddir", ".builddir"
};

static boolean scanfs_exclusion = TRUE;

void
scanfs_set_exclusion( boolean enabled )
{
	scanfs_exclusion = enabled;
}

boolean
scanfs_get_exclusion( void )
{
	return scanfs_exclusion;
}

static boolean
dir_name_excluded( const char *name )
{
	int i;

	if (!scanfs_exclusion)
		return FALSE;
	for (i = 0; i < (int)G_N_ELEMENTS(excluded_dir_names); i++)
		if (strcmp( name, excluded_dir_names[i] ) == 0)
			return TRUE;
	return FALSE;
}
```

Hook in `process_dir()`: immediately after the successful `stat_node( node )` (read the surrounding code — the stat-failed arm right there already shows the exact unlink/destroy dance):

```c
		if (NODE_IS_DIR(node)
		    && dir_name_excluded( NODE_DESC(node)->name )) {
			/* Excluded: never traversed, never in the tree --
			 * same removal the stat-failure path uses */
			g_node_unlink( node );
			g_node_destroy( node );
			continue;
		}
```

(Adjust to the loop's actual control flow — if the stat-failure arm uses something other than `continue`, mirror it; and make sure any per-node counters/ids incremented before this point are handled the same way the stat-failure path handles them.)

- [ ] **Step 4: Run the test — PASS — then whole suite** → 8/8.

- [ ] **Step 5: Vis-menu toggle (ui_main.cpp)**

In the `Vis` menu after the mode items:

```cpp
ImGui::Separator();
// Built-in scan exclusion (scanfs.c's conservative list). Toggling
// changes the tree's contents, so it queues a rescan -- same deferred
// machinery as File -> Rescan.
if (ImGui::MenuItem("Skip VCS/build dirs", nullptr,
    scanfs_get_exclusion() != 0, root_change_ok)) {
	scanfs_set_exclusion(scanfs_get_exclusion() ? FALSE : TRUE);
	save_scan_exclusion();   // nvstore, same pattern as the scale
	app_request_rescan();
}
if (ImGui::IsItemHovered())
	ImGui::SetTooltip(".git .svn .hg node_modules __pycache__ .venv .cache builddir .builddir");
```

Plus nvstore save/load (`key_scan_exclusion`, boolean — the exact `open_files_allowed` pattern), loaded at startup BEFORE the initial scan so a persisted "off" applies to the first scan too (find where `open_files_allowed` loads and check it runs pre-scan; if not, load this key earlier, right before `fsv_init`/first `scanfs`, and say where in the report). `root_change_ok` is the same guard Rescan/Change Root already use (`src/sdl/ui_main.cpp:239-242`).

- [ ] **Step 6: Build, suite, commit**

```bash
git add src/scanfs.c src/scanfs.h tests/test_scanfs_exclude.c tests/meson.build src/sdl/ui_main.cpp
git commit -m "feat: built-in scan exclusion of VCS/build dirs, on by default

Directories named .git/.svn/.hg/node_modules/__pycache__/.venv/
.cache/builddir/.builddir (exact basename, dirs only -- deliberately
no build/target/dist, too many real-content false positives) are
removed right after stat, never traversed: absent from the tree and
every view, and the scan itself gets faster. Vis -> 'Skip VCS/build
dirs' toggles it (persisted via nvstore, queues a rescan). TDD'd with
a runtime-built mkdtemp tree -- a committed .git fixture would fight
git itself; a FILE named .git is asserted to survive."
```

---

### Task 5: Acceptance captures, docs, push

**Files:**
- Modify: `TODO.md` (close BOTH upstream tickets: "Rewrite the MapV layout algorithm" and "A way to exclude directories from the scan")
- Modify: `docs/PORTING.md` (new short section recording the rework)
- Commit also: `docs/superpowers/plans/2026-08-14-mapv-squarify-scan-exclude.md` (this plan — untracked until now)

**Interfaces:**
- Consumes: everything prior; the shots directory.

- [ ] **Step 1: The acceptance capture**

`builddir/src/sdl/fsv --mapv --screenshot <workspace>/shots/final-mapv-repo.png .` from the repo root, all defaults (exclusion ON, √ scale, squarify). READ it against the spec's section 5: the checkout must read as its source tree — no dominant `.git` plane (it's excluded), near-square blocks, small files visible. Compare with `before-mapv-repo.png`.

- [ ] **Step 2: Close the TODO.md tickets**

Replace the two `- [ ]` items under "Still-relevant items from the 1999 upstream `TODO`" ("**Rewrite the MapV layout algorithm.**…" and "**A way to exclude directories from the scan**…") with checked, past-tense closures in the file's established voice, citing: `src/squarify.c` (meson test `squarify`), the √/linear/log Display option, `scanfs.c`'s built-in list + Vis toggle (meson test `scanfs_exclude`), and the design spec `docs/superpowers/specs/2026-08-14-mapv-squarify-scan-exclude-design.md`.

- [ ] **Step 3: PORTING.md section**

Add a short section after the fsn-mode material (match the doc's heading style), e.g. `## Post-v0.3: MapV squarify + scan exclusion (2026-08-14)`: 6–10 lines summarizing the three changes, the two new tests, the GTK-arm note (core defaults apply there, no GTK UI added), and a pointer to the spec.

- [ ] **Step 4: Commit and push**

```bash
git add TODO.md docs/PORTING.md docs/superpowers/plans/2026-08-14-mapv-squarify-scan-exclude.md
git commit -m "docs: close the MapV layout + scan exclusion upstream tickets

Both 1999-upstream TODO items land in one rework (squarify module,
sqrt-size default scale, built-in exclusion): point the tickets at the
code, the two new tests, and the design spec; record the rework in
PORTING.md with the GTK-arm defaults note."
git push origin fsn-mode
```

Watch CI to green (`gh run list -R w3cdotorg/fsv --branch fsn-mode --limit 1`, `gh run watch <id> -R w3cdotorg/fsv --exit-status` — remember `-R w3cdotorg/fsv`). If CI fails, report — do not attempt fixes. NOTE: the linux-gtk CI leg compiles `src/squarify.c` and the scanfs changes into the GTK build — a failure there is a real finding, report it with the log.

---

## Notes for the final review

- The three PNGs (`before-mapv-repo.png`, `after-squarify-repo.png`, `after-sqrt-repo.png`, `final-mapv-repo.png`) are the acceptance evidence — read them.
- The spec (committed `ea40a5f`) is the authority; the four user choices are recorded in its header.
- Known accepted trade-offs to verify are DOCUMENTED, not silently present: blocks now appear size-sorted (not scan-ordered); GTK arm gets the new defaults with no toggle UI.
