# MapV Squarified Treemap + Scan Exclusion — Design

Approved by Willow in chat, 2026-08-14 (~11:55 CEST), after four explicit
design choices (AskUserQuestion): squarified layout; √size default area
scale with linear/log options; built-in default exclusion list (no CLI
flag, no editing UI in V1); excluded directories not scanned at all.

Closes two "Still-relevant items from the 1999 upstream TODO" tickets in
`TODO.md`: **Rewrite the MapV layout algorithm** and **A way to exclude
directories from the scan**. The everyday motivation is the diagnosis of
2026-08-09: on a real checkout, `.git`/`.builddir` hold ~95% of the bytes,
so MapV renders one vast plane with every real file squeezed into a
paper-thin sliver.

## 1. Squarified treemap, isolated as a pure module

**Problem.** `src/geometry.c`'s `mapv_init_recursive()` lays children out
in greedy full-width rows (pass 2), with pass 3 literally
`/* Note to self: write layout optimization routine sometime */`. Result:
some nodes too wide, some too tall, frontmost rows paper-thin.

**Design.** New pure module `src/squarify.c` + `src/squarify.h`,
implementing the squarified-treemap algorithm (Bruls, Huizing, van Wijk
2000): blocks are laid in runs against the shorter side of the remaining
free rectangle; a run is closed when adding the next block would worsen
the run's worst aspect ratio. Interface (pure math, no glib beyond
basics, no gpu, no globals):

```c
typedef struct {
	double x, y;  /* rectangle origin (right/rear corner semantics
	               * are the caller's business; this module is pure
	               * 2D: origin + extents) */
	double w, h;
} SquarifyRect;

/* Lays out n areas (areas[i] > 0, any order; layout preserves input
 * order within the output array) inside `bounds`. Writes one rect per
 * area into out_rects (caller-allocated, n entries). The rects tile
 * `bounds` exactly: sum of areas is normalized to bounds' area by the
 * function, so callers pass RELATIVE areas and need not pre-scale. */
void squarify_layout( const SquarifyRect *bounds, const double *areas,
                      int n, SquarifyRect *out_rects );
```

- Added to `libfsvcore` (meson) → headlessly unit-testable.
- Internally the algorithm sorts descending by area (treemap convention,
  required for its aspect-ratio guarantee) but returns rects in the
  input's order, so the caller keeps its own node↔rect correspondence.
- Guarantees tested: exact tiling (Σ rect areas == bounds area, no
  overlaps, all inside bounds), input-order correspondence, and a
  worst-aspect-ratio sanity bound on a pathological (heavily skewed)
  input demonstrating improvement over full-width rows.

**Integration.** `mapv_init_recursive()` keeps pass 1 (block creation:
node size + border margin) and replaces passes 2–4's row machinery with
one `squarify_layout()` call over the directory's inner rectangle,
then assigns `MAPV_GEOM_PARAMS` per node from the returned rects,
re-deriving each block's exact border inset with the existing
quadratic-border formula. Heights, deployment handling, and everything
else in `geometry.c` are untouched. Visible change: blocks become
near-square and appear size-sorted (largest at the run origin) rather
than scan-ordered.

**Rejected alternative:** rewriting in place inside `geometry.c` — fewer
files, but `geometry.c` is GL-bound and outside `libfsvcore`, so the
algorithm would be untestable headlessly.

## 2. Area scale: √size default, linear/log₂ options

**Problem.** Area ∝ bytes means one 100 MB pack file visually erases ten
thousand source files.

**Design.** A single mapping applied per FILE node:

- `MAPV_SCALE_SQRT` (default): area weight = `sqrt(size)`
- `MAPV_SCALE_LINEAR`: weight = `size` (the historical look)
- `MAPV_SCALE_LOG`: weight = `log2(1 + size)`

A directory's weight is the **sum of its descendants' mapped weights**
(computed in a recursive pre-pass — mapping the sum instead would break
parent/child area consistency). The pre-pass stores the weight per node
(new `double area_weight` field in `MapVGeomParams`, computed at layout
time; no scanfs change). All places where byte sizes currently enter
MapV area math (block areas in `mapv_init_recursive()`, root dims in
`mapv_init()`) switch to weights. The `MAX(256, size)` floor becomes a
floor on the weight input, preserving its "tiny files stay visible"
intent across scales.

**UI.** `Display → MapV area scale` submenu (radio: `√ size` /
`Linear` / `Log`), enabled in every mode but only affecting MapV.
Persisted in `~/.fsvrc` via nvstore (same pattern as
`ui_dialogs.cpp`'s `open_files_allowed`: read at startup with a
default, write on change). Changing it triggers a geometry relayout of
the MapV tree (same rebuild path a colexp/deployment change uses — NOT
a rescan).

## 3. Scan exclusion: built-in list, never traversed

**Design.** In `src/scanfs.c`'s `process_dir()`, right after
`stat_node()` succeeds: if the node is a directory, exclusion is
enabled, and its **name** (exact basename match, directories only) is in
the built-in list, the node is unlinked and destroyed exactly like the
existing stat-failure path — absent from the tree and every view, its
subtree never traversed (fast). Files are never excluded by name.

**Built-in list (V1, deliberately conservative — no `build`, `target`,
`dist`: too many real-content false positives):**

```
.git .svn .hg node_modules __pycache__ .venv .cache builddir .builddir
```

Owned by `scanfs.c` as a static table with one accessor
(`scanfs_exclusion_enabled()` / `scanfs_set_exclusion()`), so the
frontend toggles it without owning the list.

**UI.** `Vis → Skip VCS/build dirs` checkbox (checked by default),
persisted via nvstore; toggling it calls `app_request_rescan()` (the
File → Rescan machinery, `src/sdl/app.h`) since the tree contents
change. Tooltip (or the menu item's own text) names the list so the
user can see what's skipped.

**Out of scope (possible follow-ups, not V1):** `--exclude` CLI flag,
user-editable list, glob patterns, per-path exclusions.

## 4. Testing & verification

- `tests/test_squarify.c` (meson test `squarify`): pure-module
  invariants — tiling exactness, in-bounds, input-order mapping,
  worst-aspect improvement on a skewed distribution vs a naive
  full-width-rows reference computed in the test.
- Area-scale mapping: the weight function is MapV policy and lives in
  `geometry.c`, which is not headless-linkable; its math (√/log/linear
  + floor) is trivial enough that its correctness is carried by the
  visual check — no dedicated unit test. The
  parent-weight-is-sum-of-children property is enforced by construction
  (single recursive pre-pass).
- `tests/test_scanfs_exclude.c` (meson test `scanfs_exclude`): builds a
  temp tree at runtime (mkdtemp) containing `.git/`, `node_modules/`
  (with content), a file literally named `.git` (must NOT be excluded —
  files never are), and normal content; asserts exclusion ON removes
  exactly the excluded dirs and OFF keeps them. scanfs.c is in
  libfsvcore, so this links like test_scanfs.
- Visual: `--mapv --screenshot` captures of THIS repo before/after
  (exclusion + √ scale + squarify) — the 2026-08-09 "sliver" case is
  the acceptance demo. Captured with the established throwaway-harness
  conventions where needed (MapV screenshots need no selection harness).
- Whole suite + CI green on both frontends. GTK arm: scanfs/squarify
  changes are core-side and build there; the two new UI controls are
  SDL-frontend-only (GTK menus untouched — GTK frontend keeps old
  behavior toggles absent, defaults apply).

## 5. Defaults interplay (the acceptance story)

On `fsv --mapv <checkout>` with everything default: `.git`/`.builddir`
are gone from the scan, √size tames the remaining skew, and squarify
gives near-square blocks — the checkout reads as its source tree, not
as one pack-file plane with a sliver.
