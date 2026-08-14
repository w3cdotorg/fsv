# FSN File-Zoom Framing Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix TODO.md bug #2 — `fsn_look_at()`'s file-zoom framing lands the camera nose-first against the box row — by framing a file at its parent directory's establishing distance while keeping the target centered on the file, TDD'd with a headless test.

**Architecture:** Root cause (investigated via systematic-debugging): in `src/camera.c`'s `fsn_look_at()`, the framing diameter is `SQRT_2 * MAX(ped->w, ped->d)` computed from the **target node's own** pedestal record — for a file that is its box's own footprint (box-scale, e.g. 64 units), so `field_distance()` puts the camera at box-row scale with no ground in frame. PORTING.md's Task B3 verification hit this on a real tree and empirically verified the legible alternative: *"Framing the camera on the parent directory instead (while leaving the actual selection on the file) gives a legible shot."* The fix implements exactly that verified framing: for a file, compute the diameter from the **parent directory's** pedestal (including the existing expanded-directory wire arm, gated on the parent), leaving the target on the file's box. Verification is headless: the new stateful dirtree stubs plus `fsv_animation_tick()` + `morph_finish()` let a test snap camera morphs to their end values and assert `camera->distance` parity between "look at dir-a" and "look at a file inside dir-a". This is the second headless test to reach `camera.c`, which triggers the ledgered promotion of the five no-op `fsv_platform` hooks into the shared shim (as an opt-in init function, so `fsv-scan` — which links the same shim — is untouched unless it calls it, answering TODO.md's own "needs thought" caveat).

**Tech Stack:** C (GLib), Meson/ninja, existing headless test pattern (libfsvcore objects + `tools/fsv-headless-stubs.c` + `tests/fixture`).

**Spec:** `TODO.md` lines 18–21 (the `fsn_look_at()` nose-first ticket) + `docs/PORTING.md`'s Task B3 "Concerns / disclosed gaps" bullet ("out of B3's scope to fix — it is `camera.c`'s framing rule") and the B3 verification note recording the parent-framing shot as legible.

## Global Constraints

- Build dir is `builddir` (default `debug` buildtype ⇒ `-DDEBUG` live in tests).
- Every shell step starts from repo root `/Users/willow/Sites/_Claude_output/fsn/fsv`; `export PATH="/opt/homebrew/bin:$PATH"` first (Homebrew meson/ninja).
- Branch: `fsn-mode` (current). Commit per task, repo message style (`test:`/`fix:`/`docs:` prefix, body explains why).
- C dialect in `tests/`/`tools/`/`src/*.c` is C89-flavored GLib C (declarations at block top, `boolean`/`TRUE`/`FALSE`, upstream `func( arg )` spacing in `src/*.c`); match each file's own style.
- The only product-code change in this plan is `fsn_look_at()`'s diameter computation (Task 2 Step 4). `fsn_warp_pose()`, targets, phi/theta, pan-time are all out of scope.
- Test names: existing `fsn_camera` keeps its name; the new test is `fsn_framing`.

---

### Task 1: Promote the fsv_platform no-op hooks into the shared headless shim

TODO.md's "Test-harness limitations" already schedules this: the five no-op `fsv_platform` hooks live in `tests/test_fsn_camera.c`'s `main()` and should be promoted "once a second headless test needs to drive `camera.c`" — Task 2's test is that second test. The caveat ("`fsv-scan` links the same shim and doesn't want a real `fsv_platform` populated underneath it") is answered by shape: an **opt-in** `fsv_headless_platform_init()` function. Nothing is populated unless a caller calls it; `fsv-scan` doesn't.

**Files:**
- Modify: `tools/fsv-headless-stubs.c` (add the five static no-ops + the init function, after the existing includes; add `#include "fsv-platform.h"`)
- Modify: `tests/test_fsn_camera.c` (delete its five local no-op functions and the five assignments; declare and call the new init)
- Modify: `TODO.md` lines 117–123 (the harness-limitation bullet: promotion is done, rewrite the bullet)

**Interfaces:**
- Consumes: `FsvPlatformHooks fsv_platform` (`src/fsv-platform.h`) — fields `request_frame(void)`, `render_frame(void)`, `viewport_size(int*,int*)`, `set_scroll(int,double,double,double,double)`, `get_scroll(int)->double`.
- Produces: `void fsv_headless_platform_init( void );` in `tools/fsv-headless-stubs.c` — Task 2's test declares it `extern` and calls it first in `main()`.

- [ ] **Step 1: Add the opt-in init to the shim**

In `tools/fsv-headless-stubs.c`, add `#include "fsv-platform.h"` to the include block, and add (its own commented section, matching the file's conventions):

```c
/* fsv-platform.h -- animation.c's frontend-hook table, which camera.c
 * and redraw( ) call through unconditionally (request_frame( ),
 * set_scroll( ), render_frame( ) via fsv_animation_tick( )). Headless
 * tests that drive camera.c need every field non-NULL; fsv-scan links
 * this same shim but never touches the camera, so the table stays
 * unpopulated for it. Deliberately OPT-IN -- a test calls
 * fsv_headless_platform_init( ) at the top of main( ) -- rather than a
 * constructor, exactly so linking the shim alone changes nothing.
 * Promoted here from tests/test_fsn_camera.c's main( ) when the second
 * camera-driving test arrived (TODO.md, test-harness limitations). */

static void
headless_request_frame( void )
{
}

static void
headless_render_frame( void )
{
}

static void
headless_viewport_size( int *width, int *height )
{
	if (width != NULL)
		*width = 800;
	if (height != NULL)
		*height = 600;
}

static void
headless_set_scroll( int axis, double lower, double upper, double page, double pos )
{
	(void)axis;
	(void)lower;
	(void)upper;
	(void)page;
	(void)pos;
}

static double
headless_get_scroll( int axis )
{
	(void)axis;
	return 0.0;
}

void
fsv_headless_platform_init( void )
{
	fsv_platform.request_frame = headless_request_frame;
	fsv_platform.render_frame = headless_render_frame;
	fsv_platform.viewport_size = headless_viewport_size;
	fsv_platform.set_scroll = headless_set_scroll;
	fsv_platform.get_scroll = headless_get_scroll;
}
```

- [ ] **Step 2: Switch test_fsn_camera.c to the shared init**

In `tests/test_fsn_camera.c`: delete the five `noop_*` function definitions (lines ~57–91) and the five `fsv_platform.* = noop_*;` assignments at the top of `main()` (lines ~98–102). In their place, above `main()`, declare:

```c
/* tools/fsv-headless-stubs.c -- opt-in fsv_platform no-op hooks (the
 * shim has no header; see its own comment for why this is a function
 * call rather than link-time population) */
extern void fsv_headless_platform_init( void );
```

and make the first statement of `main()`:

```c
	fsv_headless_platform_init();
```

Keep the explanatory comment about WHY the hooks are needed (animation.c's zero-initialized table, `redraw()`/`camera_update_scrollbars()` calling through unconditionally) — condense it to 2–3 lines above the extern declaration; the mechanics now live in the shim's comment.

- [ ] **Step 3: Update the TODO.md bullet**

Replace the TODO.md "Test-harness limitations" bullet at lines 117–123 (the one beginning `**`tests/test_fsn_camera.c`'s five no-op `fsv_platform` hooks`) with:

```markdown
- [x] ~~**`tests/test_fsn_camera.c`'s five no-op `fsv_platform` hooks live in
  that test's own `main()`**, not in `tools/fsv-headless-stubs.c`.~~
  Promoted into the shim as opt-in `fsv_headless_platform_init()` when the
  second camera-driving headless test (`fsn_framing`) arrived; `fsv-scan`
  links the same shim but is unaffected because nothing populates
  `fsv_platform` unless a test calls the init.
```

- [ ] **Step 4: Build and run the whole suite**

Run: `export PATH="/opt/homebrew/bin:$PATH" && ninja -C builddir && meson test -C builddir`
Expected: all 5 tests PASS (`scanfs`, `nvstore`, `color_persistence`, `fsn_layout`, `fsn_camera`), and `fsv-scan` still builds (ninja covers it).

- [ ] **Step 5: Commit**

```bash
git add tools/fsv-headless-stubs.c tests/test_fsn_camera.c TODO.md
git commit -m "test: promote the fsv_platform no-op hooks into the headless shim

The second headless test driving camera.c is about to land
(fsn_framing, the nose-first framing fix), which is the promotion
trigger TODO.md's harness-limitation bullet set for these five hooks.
Opt-in shape -- fsv_headless_platform_init() called from a test's
main() -- so fsv-scan, which links the same shim, keeps an unpopulated
fsv_platform exactly as before (the bullet's own caveat)."
```

---

### Task 2: TDD — failing framing test, then the fsn_look_at() fix

The test asserts the design contract directly, with no duplicated formula: after `camera_look_at(file2)` (a file inside expanded `dir-a`), `camera->distance` must equal what `camera_look_at(dir_a)` itself produces — the parent's establishing distance — while the camera target stays centered on the file's own box. Camera morphs never complete without frames, so the test snaps them: `morph_finish()` marks a morph's `t_end = 0`, and one `fsv_animation_tick()` call then assigns end values synchronously (`src/animation.c`, `morph_iteration()`).

**Files:**
- Create: `tests/test_fsn_framing.c`
- Modify: `tests/meson.build` (shared-variable dedup + the new entry)
- Modify: `src/camera.c:1248-1254` (the `fsn_look_at()` diameter block — Step 4 only, AFTER the failing run is recorded)

**Interfaces:**
- Consumes: Task 1's `fsv_headless_platform_init()`; `camera_look_at()`, `camera` global + `MAPV_CAMERA()` accessor (`src/camera.h`); `morph_finish()`, `fsv_animation_tick()` (`src/animation.h`, `src/fsv-platform.h`); `fsn_layout_get()` (`src/geometry-fsn.h`); stateful `dirtree_entry_expand()` stubs.
- Produces: meson test `fsn_framing`; the fixed framing rule in `fsn_look_at()` (files framed with the parent's diameter rule).

- [ ] **Step 1: Dedup the meson linking block and register the new test**

In `tests/meson.build`, right after the top-of-file comment, define shared variables, and rewrite the four existing `objects:`-pattern entries plus the new one to use them (`test_nvstore` keeps its own minimal form — it doesn't link libfsvcore):

```meson
# Shared linking recipe for every test that exercises libfsvcore
# headlessly (all of them except nvstore's stub-free unit test):
# core objects + tools/fsv-headless-stubs.c, and the fixture path.
core_test_deps = [glibdep, cglm_dep, libm, libmisc_dep, libdebug_dep]
fixture_c_args = ['-DFIXTURE_DIR="@0@"'.format(meson.current_source_dir() / 'fixture')]
```

Each libfsvcore-linking entry then reads (shown for the new test; apply the same shape to `test_scanfs`, `test_color_persistence`, `test_fsn_layout`, `test_fsn_camera`, preserving each one's own explanatory comment):

```meson
# Regression test for TODO.md bug #2 (fsn_look_at()'s nose-first
# file-zoom framing): a file must be framed at its parent directory's
# establishing distance -- target on the file, distance from the parent
# pedestal's footprint -- not at its own box-scale footprint.
test_fsn_framing = executable('test_fsn_framing',
  ['test_fsn_framing.c', headless_stubs_src],
  objects: libfsvcore.extract_all_objects(recursive: false),
  dependencies: core_test_deps,
  include_directories: incdir,
  c_args: fixture_c_args)
test('fsn_framing', test_fsn_framing)
```

- [ ] **Step 2: Write the failing test**

Create `tests/test_fsn_framing.c`:

```c
/* tests/test_fsn_framing.c
 *
 * SPDX-License-Identifier: MIT
 *
 * Regression test for TODO.md bug #2: fsn_look_at( )'s file-zoom
 * framing computed its diameter from the target FILE's own pedestal
 * record -- the file box's own footprint, box-scale -- so a real
 * click-to-fly on a file in a densely packed directory put the camera
 * nose-first against the box row with no ground in frame (PORTING.md,
 * Task B3 verification: framing the camera on the PARENT directory
 * instead, selection left on the file, was the verified legible shot).
 *
 * The contract pinned here is exactly that verified framing, asserted
 * without duplicating any camera formula: looking at a file must
 * produce the SAME camera->distance as looking at its parent directory
 * (parent-footprint diameter rule, expanded-wire arm included), while
 * the camera target stays centered on the file's own box.
 *
 * MECHANICS. Camera pose changes are morphs, and no frames ever run
 * headlessly, so end values are snapped: morph_finish( ) zeroes a
 * morph's t_end and the next fsv_animation_tick( ) assigns its end
 * value synchronously (src/animation.c, morph_iteration( )).
 *
 * LINKING. Same recipe as test_fsn_camera: libfsvcore objects +
 * tools/fsv-headless-stubs.c (stateful dirtree flags, opt-in
 * fsv_platform no-op hooks). */

#include <assert.h>
#include <math.h>
#include <string.h>

#include "common.h" /* pulls in glib.h, and must precede it: G_LOG_DOMAIN */
#include "fsv.h"
#include "animation.h"
#include "camera.h"
#include "dirtree.h"
#include "fsv-platform.h"
#include "geometry-fsn.h"
#include "scanfs.h"

#define TOL 1.0e-9

/* tools/fsv-headless-stubs.c -- opt-in fsv_platform no-op hooks (the
 * shim has no header of its own) */
extern void fsv_headless_platform_init( void );

static GNode *
child_named(GNode *parent, const char *name)
{
	GNode *node;

	for (node = parent->children; node != NULL; node = node->next)
		if (strcmp(NODE_DESC(node)->name, name) == 0)
			return node;

	return NULL;
}

/* Snap the camera-pose morphs camera_look_at( ) schedules to their end
 * values: morph_finish( ) is a no-op for a variable not being morphed,
 * so finishing more than was scheduled is safe. */
static void
snap_camera_morphs(void)
{
	morph_finish(&camera->distance);
	morph_finish(&camera->theta);
	morph_finish(&camera->phi);
	morph_finish(&MAPV_CAMERA(camera)->target.x);
	morph_finish(&MAPV_CAMERA(camera)->target.y);
	morph_finish(&MAPV_CAMERA(camera)->target.z);
	fsv_animation_tick();
}

int
main(void)
{
	GNode *root, *dir_a, *file2;
	const FsnPedestal *fped;
	double dir_distance;

	fsv_headless_platform_init();

	scanfs(FIXTURE_DIR); /* FIXTURE_DIR injected by meson (-D) */
	assert(globals.fstree != NULL);

	root = root_dnode;
	assert(root != NULL);
	dir_a = child_named(root, "dir-a");
	assert(dir_a != NULL && NODE_IS_DIR(dir_a));
	file2 = child_named(dir_a, "file2.bin");
	assert(file2 != NULL && !NODE_IS_DIR(file2));

	globals.fsv_mode = FSV_FSN;
	fsn_geometry_init(root);
	camera_init(FSV_FSN, TRUE);

	/* The framing case users actually hit: the parent directory is
	 * expanded (its file boxes packed on the pedestal top) and a file
	 * box is clicked. Expanded also engages the wire arm of the
	 * directory diameter rule, so the parity assert below covers the
	 * whole rule, not just the SQRT_2 footprint half. */
	dirtree_entry_expand(dir_a);
	assert(dirtree_entry_expanded(dir_a));

	/* Establishing shot of the parent itself: the distance every
	 * fsn_look_at( ) caller gets for dir-a's own footprint. */
	camera_look_at(dir_a);
	snap_camera_morphs();
	dir_distance = camera->distance;
	assert(dir_distance > 0.0);

	/* Now the bug's exact gesture: click-to-fly on a file inside it. */
	camera_look_at(file2);
	snap_camera_morphs();

	/* Target stays centered on the file's own box... */
	fped = fsn_layout_get(file2);
	assert(fped != NULL);
	assert(fabs(MAPV_CAMERA(camera)->target.x - fped->x) < TOL);
	assert(fabs(MAPV_CAMERA(camera)->target.y - fped->z) < TOL);

	/* ...but the camera stands at the PARENT's establishing distance,
	 * not at the file box's own box-scale one. Pre-fix, this is the
	 * nose-first bug: distance derived from the file's tiny footprint,
	 * far short of the parent's. */
	assert(fabs(camera->distance - dir_distance) < TOL);

	return 0;
}
```

- [ ] **Step 3: Run the new test — must FAIL (RED)**

Run: `export PATH="/opt/homebrew/bin:$PATH" && ninja -C builddir && meson test -C builddir fsn_framing --print-errorlogs`
Expected: FAIL on the final `assert(fabs(camera->distance - dir_distance) < TOL)` — the current code frames the file at its own box-scale distance, well short of the parent's. If it instead fails on an EARLIER assert (target/setup), fix the test's setup, re-run until the failure is exactly the distance-parity assert, and record that output. Do not touch `src/camera.c` yet.

- [ ] **Step 4: Implement the fix (GREEN)**

In `src/camera.c`, `fsn_look_at()`: add `const FsnPedestal *fped;` and `GNode *fnode;` to the declarations, and replace the diameter block (currently lines 1248–1254):

```c
	/* Enough of the object in frame to make it identifiable -- and, for
	 * an expanded directory, enough to take in the wires leaving it and
	 * the near edge of the generation they lead to, which is the whole
	 * point of the mode */
	diameter = SQRT_2 * MAX(ped->w, ped->d);
	if (NODE_IS_DIR(node) && dirtree_entry_expanded( node ))
		diameter = MAX(diameter, ped->d + 2.0 * FSN_GENERATION_GAP);
```

with:

```c
	/* Enough of the object in frame to make it identifiable -- and, for
	 * an expanded directory, enough to take in the wires leaving it and
	 * the near edge of the generation they lead to, which is the whole
	 * point of the mode.
	 *
	 * A FILE is framed by its PARENT's footprint, not its own: a file
	 * box's own record is box-scale, and sizing the shot to it put the
	 * camera nose-first against the packed box row (TODO.md bug #2;
	 * PORTING.md's Task B3 verification recorded parent-framing, with
	 * the selection left on the file, as the legible shot). The target
	 * above stays on the file's box -- only the framing scale changes. */
	fnode = node;
	fped = ped;
	if (!NODE_IS_DIR(node) && node->parent != NULL &&
	    NODE_IS_DIR(node->parent) &&
	    fsn_layout_get( node->parent ) != NULL) {
		fnode = node->parent;
		fped = fsn_layout_get( node->parent );
	}
	diameter = SQRT_2 * MAX(fped->w, fped->d);
	if (NODE_IS_DIR(fnode) && dirtree_entry_expanded( fnode ))
		diameter = MAX(diameter, fped->d + 2.0 * FSN_GENERATION_GAP);
```

(The expanded-wire arm now keys on `fnode` — for a file, the parent — giving exact parity with `camera_look_at(parent)`, which is what the test asserts.)

- [ ] **Step 5: Run the new test — must PASS, then the whole suite**

Run: `export PATH="/opt/homebrew/bin:$PATH" && ninja -C builddir && meson test -C builddir fsn_framing --print-errorlogs && meson test -C builddir`
Expected: `fsn_framing` PASS, then all 6 tests PASS (the framing change must not disturb `fsn_camera` — it asserts flags and `current_node`, not distances).

- [ ] **Step 6: Commit**

```bash
git add tests/test_fsn_framing.c tests/meson.build src/camera.c
git commit -m "fix(fsn): frame a file look-at by its parent's footprint, not its own

fsn_look_at()'s diameter came from the target node's own pedestal
record; for a file that is the box's own box-scale footprint, so
click-to-fly on a file in a densely packed directory landed the camera
nose-first against the box row with no ground in frame (TODO.md bug 2,
surfaced by Task B3's verification -- which also recorded the fix:
framing on the parent, selection left on the file, is the legible
shot). Files now use the parent directory's diameter rule, expanded
wire arm included, target unchanged on the file's box.

TDD'd by tests/test_fsn_framing.c (meson test fsn_framing): asserts
distance parity with camera_look_at(parent) and the target staying on
the file, snapping camera morphs headlessly via morph_finish() +
fsv_animation_tick(). tests/meson.build's per-test linking recipe
dedup'd into core_test_deps/fixture_c_args while adding the sixth
copy."
```

---

### Task 3: Close the ticket, sync docs, push

**Files:**
- Modify: `TODO.md:18-21` (the bug #2 item)
- Modify: `docs/PORTING.md` (the Task B3 "Concerns / disclosed gaps" bullet beginning `**`fsn_look_at()`'s file-zoom diameter (Task B1, not touched here)` at ~line 4957 — grep for `nose-first` to locate both mentions; only this bullet gets the closure note, the ~4742 verification narrative stays historical as-is)

**Interfaces:**
- Consumes: Task 2's commit hash (fill in after it exists), test file `tests/test_fsn_framing.c`, meson test name `fsn_framing`.
- Produces: docs only.

- [ ] **Step 1: Mark the TODO.md ticket resolved**

Replace TODO.md lines 18–21 (the `- [ ] **`fsn_look_at()` file-zoom framing…` item) with:

```markdown
- [x] ~~**`fsn_look_at()` file-zoom framing lands the camera nose-first against
  the box row** when click-to-fly targets a file in a densely packed
  directory.~~ **Fixed**: a file is now framed at its parent directory's
  establishing distance (parent-footprint diameter rule, expanded wire arm
  included), target left on the file's box — the exact framing Task B3's
  verification recorded as the legible shot. Regression-locked by
  `tests/test_fsn_framing.c` (meson test `fsn_framing`).
```

- [ ] **Step 2: Append the closure note to PORTING.md's B3 gap bullet**

Locate the "Concerns / disclosed gaps" bullet in the Task B3 section (grep `nose-first`; the bullet ends `…it is \`camera.c\`'s framing rule, not the decal.)`). Append to that bullet, matching the doc's voice:

```markdown
  **Update (post-v0.3): fixed.** `fsn_look_at()` now frames a file by its
  parent directory's footprint (wire arm included), target unchanged on the
  file — the parent-framing shot this very verification recorded as legible.
  Regression-locked by `tests/test_fsn_framing.c` (meson test `fsn_framing`).
```

- [ ] **Step 3: Commit and push**

```bash
git add TODO.md docs/PORTING.md
git commit -m "docs: close TODO's nose-first framing ticket (fixed + regression-locked)

Bug 2's fix lands the framing Task B3's own verification already
recorded as the legible shot; point the ticket and PORTING.md's B3 gap
bullet at the fix and the fsn_framing test."
git push origin fsn-mode
```

Then watch CI to green: `gh run list -R w3cdotorg/fsv --branch fsn-mode --limit 1`, `gh run watch <id> -R w3cdotorg/fsv --exit-status`. If CI fails, report — do not attempt fixes.

---

## Optional follow-up (not a task — needs a human)

30-second visual QA: `builddir/src/sdl/fsv --fsn src` (or any dense tree), expand a directory, single-click one of its file boxes. Expected: the camera settles at the parent's establishing distance with the file's box centered and the spotlight pooled at its base — ground and neighboring boxes in frame, no nose-first wall of boxes.
