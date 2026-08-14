# TODO — deferred tickets from the macOS/Metal port

Small deferred items disclosed throughout [`docs/PORTING.md`](docs/PORTING.md)
("Concerns / disclosed gaps" sections and inline notes), collected here as
actionable tickets. The historical upstream wishlist lives in [`TODO`](TODO).

## Known bugs (highest priority)

- [x] ~~**Clicking a file child of a *collapsed* directory aborts DEBUG builds
  (FSN mode).**~~ **Was already fixed** when this file was first written —
  commit fa9383c (final Milestone C review) added
  `fsn_ensure_parent_expanded()` in `src/camera.c`, centralized ahead of
  the DEBUG assertions in both `camera_look_at_full()` and
  `camera_warp_to()`; this ticket was collected from PORTING.md's earlier
  "disclosed gaps" note without noticing the later fix round. Now
  regression-locked by `tests/test_fsn_camera.c` (meson test
  `fsn_camera`), which also made the headless dirtree stubs stateful.
- [x] ~~**`fsn_look_at()` file-zoom framing lands the camera nose-first against
  the box row** when click-to-fly targets a file in a densely packed
  directory.~~ **Fixed**: a file is now framed at its parent directory's
  establishing distance (parent-footprint diameter rule, expanded wire arm
  included), target left on the file's box — the exact framing Task B3's
  verification recorded as the legible shot. Regression-locked by
  `tests/test_fsn_framing.c` (meson test `fsn_framing`) — verified
  numerically (distance parity + target-on-file); the headed 30-second QA
  on a dense tree is still pending.
- [ ] **"Night" landscape preset was never calibrated** (Task A1 disclosed
  gap); auto-landscape only ever selects "classic", so it's currently
  unreachable in practice but still ships uncalibrated.
- [ ] **"Long way round" theta on birds-eye return in DiscV/MapV/TreeV** after
  a manual revolve. Fixed for FSV_FSN with `unwrap_theta_toward()`; other
  modes' pre-existing behavior explicitly left open. (Task B2 fix-round note)

## Docs / demo

- [x] ~~**Refresh the README demo video with FSN mode.**~~ **Done**:
  `docs/media/demo.gif` / `docs/media/demo.mp4` pre-dated the FSN landscape
  work entirely and showed a MapV/TreeV tour instead. Re-recorded via
  `--record` / `tools/make-demo.sh` with a new ~24s script: the FSN intro
  pan, an `sdl/` pedestal expansion, a file look-at on `sdl/gpu.cpp`
  (parent-distance framing + the selection spotlight beam), a warp-lite
  swoop, bird's-eye (mirroring the Camera Rail's button), and a finale on
  the squarified, sqrt-scaled MapV with the built-in scan exclusion active
  (no `.git` block).

## Build / distribution debt

- [ ] **Bundle runtime dylibs into `Contents/Frameworks`**
  (`install_name_tool` / `dylibbundler`). The `.app` and tarball currently
  require `brew install glib sdl3`; bundling is the real fix for distributing
  to users without Homebrew. (PORTING.md, Task 6.2/6.3 notes)
- [ ] **Legacy GTK/OpenGL build fails on macOS** at `src/ogl.c`
  (`GL/glu.h` not found). Pre-existing; reserved for a future OpenGL-removal
  task. (PORTING.md, Task 1.1 notes)
- [ ] **`discv_get_scrollbar_state()` is a `/* TODO */` stub** that just
  echoes back whatever `scroll_state[]` already held. (PORTING.md, camera
  scrollbar notes)

## UX rough edges (accepted as YAGNI, documented)

- [ ] **Marks panel (Task C2):** no de-duplication on "Mark here" (marking the
  same node twice creates two identical rows); only one row renamable at a
  time (single `g_editing_index`); a missing mark's tooltip shows the raw
  stored path rather than a display-safe form.
- [ ] **File-open confirm modal (Task C3):** fixed corner position, never
  moves once chosen (`ImGuiCond_Always`); relax to `ImGuiCond_FirstUseEver`
  if a remembered/cascading position is ever wanted.
- [ ] **Selection spotlight is not deployment-aware (Task B3; applies to the light cone too):** during an
  ancestor's expand/collapse morph it draws at the node's static layout
  height — brief visible glitch only during the animation window.
- [ ] **Spotlight beam vanishes when the camera is inside the cone:** the
  scene pipeline back-face culls, so a camera positioned inside the light
  cone's radius sees only its back faces and the beam disappears entirely
  — reachable via warp-lite (Task C4)'s `camera_warp_to()`. Benign (the
  rest of the scene renders fine), but the intended fix is a fade-on-entry
  guard: skip or fade the cone once the camera's horizontal distance to
  its axis is inside the cone's radius at the camera's height.
- [ ] **Overview mini-map (Task C1):** framing ignores the camera — a camera
  far outside the landscape pins its marker to the frame edge instead of
  zooming out, so the marker's distance is unreadable while clamped. Also
  the overview never appears in `--screenshot` output (ImGui window; that
  path renders the scene alone), so it can't be regression-checked headlessly.
- [ ] **Warp-lite (Task C4):** `FSN_WARP_HEIGHT_LIFT` is a fixed tuned guess,
  not computed from the children's actual box heights; `colexp()`'s internal
  re-pan can be cancelled by the warp that follows it (benign — the warp is
  the pan the user asked for); no Search panel / full in-directory warp
  paradigm from upstream fsn.
- [ ] **Scan exclusion is exact-basename-only (2026 rework):** no
  prefix/glob patterns (`build*`, `*.egg-info`), no user-editable
  exclusion list (the set in `scanfs.c` is a compile-time constant), and
  no `--exclude`/`--no-exclude` CLI flag — the only override today is the
  SDL Vis-menu toggle or the GTK-only `FSV_NO_EXCLUDE` environment
  variable, both all-or-nothing. A checkout with a differently-named
  build directory (`builddir-sdl`, `build/`, `target/`, `dist/`) or any
  other dot-directory the fixed list doesn't name still scans and renders
  in full.

## Still-relevant items from the 1999 upstream `TODO`

Triaged from [`TODO`](TODO). Already resolved by the port: README content
(Task 6.5 rewrite) and the `~/.fsvrc` config backend (Task 5.3 implemented
the `lib/nvstore.c` stub for real). Obsolete with the SDL/ImGui frontend:
the Glade/GTK-cruft rewrite and the `/bin/sh` online-help hack (the port
has native Help → Controls / About windows). Still worth doing:

- [x] ~~**Rewrite the MapV layout algorithm.**~~ **Fixed**: `geometry.c`'s
  greedy row layout is replaced by a pure squarified-treemap module
  (`src/squarify.c`, Bruls et al. 2000 — meson test `squarify`), and the
  area scale defaults to √size instead of linear bytes, with linear/log₂
  as Display-menu alternatives (persisted, loaded before the first scan
  in `ui_dialogs_init()`). Together they fix both original complaints —
  no more paper-thin frontmost rows, no more one node dwarfing the rest.
  Design: `docs/superpowers/specs/2026-08-14-mapv-squarify-scan-exclude-design.md`.
- [ ] **Smarter pointing/selection for faraway nodes** (e.g. birds-eye
  view): picking returns exactly the node under the hotspot even when it's
  sub-pixel small; should walk up to an ancestor above a pixel-based
  minimum. Still true of the port's color-ID `gpu_pick()` — the Task C4
  harness's stray-click-on-a-tiny-file-box episode is this exact failure
  mode in miniature.
- [ ] **Scan caching** (`~/.fsvcache/@usr@lib`-style): store the tree state,
  re-scan only subdirectories with updated timestamps on the next launch;
  would also let ColorByTimestamp use the previous scan as the spectrum
  start ("graphical diff" of the filesystem).
- [x] ~~**A way to exclude directories from the scan**~~ **Fixed**:
  `scanfs.c` now carries a built-in, deliberately conservative
  exclusion list (**exact basename match only**, directories only —
  `.git`, `.svn`, `.hg`, `node_modules`, `__pycache__`, `.venv`,
  `.cache`, `builddir`, `.builddir`); excluded directories are never
  traversed and never enter the tree. Default on, with a Vis-menu
  toggle (meson test `scanfs_exclude`); the GTK arm has no such toggle,
  so it instead honors an `FSV_NO_EXCLUDE` environment variable as its
  off switch. Together with the MapV item above, a checkout with
  `.git`/`builddir` now renders as its source tree instead of one
  dominant plane with everything else squeezed into a sliver — note
  the exact-match caveat above: a differently-named build directory
  (`builddir-sdl`, `build/`, `target/`, `dist/`) or any other
  dot-directory (`.superpowers`) is NOT covered by this list and still
  scans and renders normally. Design:
  `docs/superpowers/specs/2026-08-14-mapv-squarify-scan-exclude-design.md`.

## Test-harness limitations (not product code)

- [ ] The Color Setup **Gradient spectrum was never driven through the
  "By date/time" tab's `Combo` dropdown in the automated harness** (an open
  `ImGui::Combo()` popup eats the next click as dismiss); verified end-to-end
  only with the tab's default Rainbow spectrum. Would need keyboard nav or a
  `BeginCombo`/`Selectable` rewrite to automate. (Task 5.3 gaps)
- [ ] **ImGui overlay pixels aren't screenshot-verifiable in the sandbox**
  (no compositor capture); dialogs are verified via internal-state tracing
  plus the offscreen scene screenshot only.
- [ ] **Task C4's MapV regression check is verified by code diff**, not a
  clean empirical click-through (synthetic-harness picking limitation at the
  one fixed pixel used in that mode's layout).
- [x] ~~**`tests/test_fsn_camera.c`'s five no-op `fsv_platform` hooks live in
  that test's own `main()`**, not in `tools/fsv-headless-stubs.c`.~~
  Promoted into the shim as opt-in `fsv_headless_platform_init()` when the
  second camera-driving headless test (`fsn_framing`) arrived; `fsv-scan`
  links the same shim but is unaffected because nothing populates
  `fsv_platform` unless a test calls the init.
