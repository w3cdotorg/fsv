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
- [x] ~~**"Night" landscape preset was never calibrated** (Task A1 disclosed
  gap); auto-landscape only ever selects "classic", so it's currently
  unreachable in practice but still ships uncalibrated.~~ **Calibrated**: the
  Night sky colors (`sky_top`, `sky_horizon`) were determined by screenshot
  iteration against a four-constraint brief (readable navy, legible geometry,
  visible spotlight beam against sky, unchanged ground); validated at both the
  default FSN establishing framing (~14% of the gradient visible) and a tilted
  sanity capture (~46% visible, near-level camera). The ground color (`rgb
  65,140,90`) is unchanged from "classic" and is load-bearing for the overview
  mini-map's pin feature.
- [x] ~~**"Long way round" theta on birds-eye return in DiscV/MapV/TreeV** after
  a manual revolve. Fixed for FSV_FSN with `unwrap_theta_toward()`; other
  modes' pre-existing behavior explicitly left open. (Task B2 fix-round note)~~
  **Closed**: `unwrap_theta_toward()` is now called at six re-pose sites,
  each immediately before that site's theta morph -- MapV's and TreeV's
  bird's-eye going-up arms, the shared bird's-eye going-down restore (now
  unconditional across all four modes, DiscV included), `mapv_look_at()`,
  `treev_look_at()`, and `camera_treev_lpan_look_at()`. DiscV's own
  bird's-eye going-up arm is the deliberate exception: theta is inert to
  DiscV's pose math there, so it is initialized defensively (rather than
  unwrapped toward anything) per the F1 fix in the fsn-mode final review.
  Regression-locked by meson test `camera_theta`.

**The Known-bugs section is now fully closed.**

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

- [x] **Bundle runtime dylibs into `Contents/Frameworks`** — done.
  `packaging/macos/make-bundle.sh` now computes the transitive non-system
  dylib closure via `otool -L` (no hard-coded list), copies it into
  `Contents/Frameworks`, rewrites ids/loads to `@rpath` and adds an
  `@executable_path/../Frameworks` rpath with `install_name_tool`, strips
  any stray absolute `LC_RPATH` entries left over from the link (these
  don't show in `otool -L`, only `otool -l`, and could otherwise let dyld
  silently fall back to a Homebrew copy on a machine that happens to have
  one), and re-signs everything ad-hoc inside-out. On by default;
  `--no-bundle-dylibs` keeps the old Homebrew-dependent behavior for local
  dev iteration. CI runs this on the macOS runner (the only one with the
  Homebrew deps the closure walk needs) and gates on a same-job audit —
  `otool -l` across the bundled binary and every `Contents/Frameworks`
  dylib, failing the job on any `/opt/homebrew` match — so the tagged
  release's `fsv.app` is self-contained as a CI-enforced fact, not a
  hoped-for one. (PORTING.md, Task 6.2/6.3 notes; `.github/workflows/ci.yml`)
- [ ] **Legacy GTK/OpenGL build fails on macOS** at `src/ogl.c`
  (`GL/glu.h` not found). Pre-existing; reserved for a future OpenGL-removal
  task. (PORTING.md, Task 1.1 notes)
- [ ] **`discv_get_scrollbar_state()` is a `/* TODO */` stub** that just
  echoes back whatever `scroll_state[]` already held. (PORTING.md, camera
  scrollbar notes)

## UX rough edges (accepted as YAGNI, documented)

- [x] ~~**Marks panel (Task C2): no de-duplication on "Mark here."**~~
  **Closed**: marking an already-bookmarked node no longer pushes a
  duplicate row — the button now walks `g_marks` for the same stored
  `node_absname()` path and, on a match, focuses that row's inline rename
  field (the same single-`g_editing_index` mechanism the row's own
  double-click-to-rename entry point uses) instead of adding, so the
  click still gives visible feedback.
- [ ] **Marks panel (Task C2), remaining:** only one row renamable at a
  time (single `g_editing_index`); a missing mark's tooltip shows the raw
  stored path rather than a display-safe form. Both still accepted-YAGNI
  per the brief's "keep it simple" instruction (PORTING.md's Task C2
  disclosed-gaps note).
- [ ] **File-open confirm modal (Task C3):** fixed corner position, never
  moves once chosen (`ImGuiCond_Always`); relax to `ImGuiCond_FirstUseEver`
  if a remembered/cascading position is ever wanted.
- [ ] **Selection spotlight is not deployment-aware (Task B3; applies to the light cone too):** during an
  ancestor's expand/collapse morph it draws at the node's static layout
  height — brief visible glitch only during the animation window.
- [x] ~~**Spotlight beam vanishes when the camera is inside the cone.**~~
  **Fixed**: the beam's alpha now fades over a smoothstep band of
  camera-distance-to-axis vs cone radius at camera height, skipping the
  draw entirely once fully inside, instead of snapping off the instant
  the camera crosses the cone wall (reachable in practice via warp-lite
  (Task C4)'s `camera_warp_to()`). The ground pool is deliberately left
  unfaded — it reads fine from inside. The band itself is calibrated to
  the *observed* vanish boundary (ratio 2.0-2.6), not the geometric cone
  wall (0.85-1.15) the fix originally targeted, because the cone's
  geometry turned out to stop rendering well outside that wall for an
  undiagnosed reason — see the next ticket below, which stays open until
  that's root-caused. Verified with outside/edge/inside posed captures.
- [ ] **Spotlight cone stops rendering entirely below ratio ~2.0-2.3, at
  full alpha, cause undiagnosed (Task 2 fix-round finding):** independent
  of the fade-on-entry band above (`FSN_SPOTLIGHT_CONE_FADE_INNER`/`_OUTER`,
  fsn-style.h) -- the cone's geometry drops out of the rendered frame
  entirely once the camera's ratio (ground distance from the cone's axis /
  cone radius at the camera's height) falls below roughly 2.0-2.3, well
  outside the geometric cone wall (ratio 1.0) the fade band was originally
  calibrated to bracket. Confirmed **not** back-face culling: forcing the
  spotlight's depth test from `FSV_DEPTH_LESS_NOWRITE` to
  `FSV_DEPTH_ALWAYS_NOWRITE` made the geometry flash fully white at ratios
  where it was otherwise invisible, proving the triangles reach the
  rasterizer and are only failing the depth *comparison*. Confirmed
  independent of how the pose is built (reproduced with
  `camera_look_at()`-style, `camera_warp_to()`-style, and a from-scratch
  pose with the look-at target decoupled from the eye-to-axis distance)
  and reproducible across two very differently scaled pedestals
  (radius-294 and radius-110), a strong signal it's ratio-driven rather
  than distance- or scale-driven. Root cause undiagnosed -- a leading
  theory involving near/far clip interaction with the cone's own tall,
  close-up geometry did not reproduce or disappear under direct
  manipulation of the clip planes, which weakens but doesn't rule it out.
  The fade band was recalibrated (2.0-2.6) to bracket this *observed*
  boundary instead of the geometric wall, so the user-facing symptom is a
  dissolve rather than a snap, but that masks rather than fixes the
  underlying invisibility -- once root-caused, retune the band back
  toward 0.85-1.15, the original theoretical target.
- [x] ~~**Overview mini-map (Task C1): framing ignores the camera.**~~
  **Closed**: the framed rect now grows toward the camera, per axis, up to
  `FSN_OVERVIEW_MAX_GROWTH` (3.0x the landscape's larger dimension) —
  beyond the cap the old edge-clamp remains the fallback — so the marker
  stays inside the frame with readable separation instead of pinned flush
  to the edge. `overview_fit_aspect()`'s fixed 512x320 stretch is applied
  after the cap, which can itself push a capped axis up to ~1.6x past
  `FSN_OVERVIEW_MAX_GROWTH` on a diagonal-outside pose — bounded and
  accepted, documented inline. FSN's own establishing shot already
  exercises this growth path by a modest amount (it looks down at the
  whole tree from behind and above, so its ground-projected eye point
  sits outside the pedestals' footprint by construction); that startup
  growth is accepted as the new baseline (2026-08-19, user decision), not
  a regression to chase with a no-growth-at-startup deadband — the true
  no-growth case is a camera over or near the landscape itself. Also now
  consumes Task 2's `camera_ground_position()` accessor in
  `draw_overview_marker()` instead of its own inlined copy of the same
  derivation. Verified via `--record` frame extraction at
  inside/outside/beyond-cap poses.
- [ ] **Overview mini-map (Task C1), remaining:** the overview never
  appears in `--screenshot` output (ImGui window; that path renders the
  scene alone), so it still can't be regression-checked headlessly.
- [ ] **Warp-lite (Task C4):** `FSN_WARP_HEIGHT_LIFT` is a fixed tuned guess,
  not computed from the children's actual box heights; `colexp()`'s internal
  re-pan can be cancelled by the warp that follows it (benign — the warp is
  the pan the user asked for); no Search panel / full in-directory warp
  paradigm from upstream fsn.
- [x] ~~**Scan exclusion is exact-basename-only (2026 rework).**~~ **Fixed**:
  the built-in list is now `fnmatch(3)` glob patterns anchored on the
  basename (`builddir*`/`.builddir*` replace the two exact names), and a
  repeatable `--exclude PATTERN` CLI flag appends user patterns onto the
  same list, gated by the same Vis-menu toggle and `FSV_NO_EXCLUDE`
  escape hatch as the built-ins — directories only, no new dependencies.
  Regression-locked by the extended `scanfs_exclude` meson test (a glob
  match, a pinned near-miss, a runtime `--exclude` pattern, and the
  exclusion-off case).

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
  **Update (2026-08-19):** the exact-match caveat above is closed — see
  the "Scan exclusion is exact-basename-only" ticket below.

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
