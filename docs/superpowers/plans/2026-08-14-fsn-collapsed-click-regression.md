# FSN Collapsed-Click Regression Lock Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Lock in the already-landed fix for TODO.md's bug #1 (clicking a file child of a collapsed directory aborted DEBUG builds in FSN mode) with a headless regression test, then close the stale ticket.

**Architecture:** The bug itself was fixed by commit `fa9383c` (`fsn_ensure_parent_expanded()` in `src/camera.c`, centralized ahead of the DEBUG assertions in both `camera_look_at_full()` and `camera_warp_to()`), but that fix landed as a code-only commit — no test, no PORTING.md verification entry — and TODO.md (written afterwards, from PORTING.md's pre-fix "disclosed gaps" sections) still lists it as open. This plan (1) makes the shared headless stubs' dirtree state *stateful* (mirroring the real SDL implementation's `DirNodeDesc::tnode` flag), which is the missing prerequisite for testing camera-vs-dirtree interactions headlessly, (2) adds a regression test that exercises both guarded camera entry points against a flag-collapsed parent and proves the test would have caught the original bug, and (3) closes the TODO.md ticket with pointers to the fix and the test.

**Tech Stack:** C (GLib), Meson/ninja, existing headless test pattern (`libfsvcore` objects + `tools/fsv-headless-stubs.c`, fixture tree in `tests/fixture`).

**Spec:** `TODO.md` (first "Known bugs" ticket) + `src/camera.c:1524-1569` (the fix and its doc comment) — this plan verifies and closes that ticket rather than re-implementing it.

## Global Constraints

- Build dir is `builddir` (exists; default `debug` buildtype ⇒ `-DDEBUG` is active, so `g_assert()` aborts are live in tests — required for the regression test to be meaningful).
- Every shell step starts from repo root `/Users/willow/Sites/_Claude_output/fsn/fsv` and may need `export PATH="/opt/homebrew/bin:$PATH"` first (Homebrew meson/ninja).
- Branch: `fsn-mode` (current). Commit per task; message style follows repo convention (`test:`, `docs:` prefixes, body explains why).
- C dialect in `tests/` and `tools/` is C89-flavored GLib C (declarations at block top, `boolean`/`TRUE`/`FALSE` from `common.h`); match it.
- Do not modify `src/camera.c` (except the temporary, reverted swap in Task 2 Step 4) — the fix under test must ship unchanged.

---

### Task 1: Stateful dirtree stubs in the headless shim

The shared stub `dirtree_entry_expanded()` in `tools/fsv-headless-stubs.c` always returns `FALSE`, so any headless test that reaches `camera_look_at_full()`'s DEBUG assert would abort even with the fix present (after `fsn_ensure_parent_expanded()` expands the parent, the stub would still report it collapsed). Replace the five dirtree no-ops with stateful implementations that mirror the real SDL frontend (`src/sdl/ui_panels.cpp:88-283`): `DirNodeDesc::tnode` repurposed as a NULL / non-NULL "expanded" flag.

**Files:**
- Modify: `tools/fsv-headless-stubs.c:47-86` (the `/* dirtree.h */` section)

**Interfaces:**
- Consumes: `DIR_NODE_DESC()`, `NODE_IS_DIR()` (`src/common.h`), `boolean`/`TRUE`/`FALSE`.
- Produces: stateful `dirtree_entry_new()`, `dirtree_entry_expanded()`, `dirtree_entry_expand()`, `dirtree_entry_expand_recursive()`, `dirtree_entry_collapse_recursive()` — exact semantics Task 2's test relies on: a directory starts expanded iff `g_node_depth(dnode) <= 2`; `collapse_recursive` clears only the node's own flag; `expand` sets the node's flag and every `NODE_IS_DIR` ancestor's.

- [ ] **Step 1: Replace the dirtree stub section**

In `tools/fsv-headless-stubs.c`, replace the bodies of the five functions under the `/* dirtree.h */` comment (keep `dirtree_clear()` and `dirtree_no_more_entries()` as no-ops) with:

```c
/* dirtree.h -- stateful, not no-op: camera.c's fsn_ensure_parent_expanded( )
 * (the fa9383c collapsed-parent fix) and its DEBUG assertions read and
 * mutate real expansion state, so tests covering them need real semantics.
 * Mirrors the SDL frontend (src/sdl/ui_panels.cpp): DirNodeDesc::tnode --
 * the field dirtree.c used for its GtkTreePath, NULLed once by scanfs.c
 * before the first dirtree_entry_new( ) call -- repurposed as a NULL /
 * non-NULL "expanded" flag, so no header changes. */

static void
set_tree_row_expanded( GNode *dnode, boolean expanded )
{
	DIR_NODE_DESC(dnode)->tnode = expanded ? (void *)1 : NULL;
}

static void
expand_ancestors( GNode *dnode )
{
	GNode *p;

	for (p = dnode->parent; (p != NULL) && NODE_IS_DIR(p); p = p->parent)
		set_tree_row_expanded( p, TRUE );
}

static void
expand_subtree_recursive( GNode *dnode )
{
	GNode *c;

	set_tree_row_expanded( dnode, TRUE );
	for (c = dnode->children; c != NULL; c = c->next) {
		if (!NODE_IS_DIR(c))
			break; /* dirs sort first -- scanfs.c's compare_node */
		expand_subtree_recursive( c );
	}
}

void
dirtree_clear(void)
{
}

void
dirtree_entry_new(GNode *dnode)
{
	/* Same initial policy as both real frontends: only the metanode
	 * (depth 1) and the scanned root (depth 2) start open */
	set_tree_row_expanded( dnode, g_node_depth( dnode ) <= 2 );
}

void
dirtree_no_more_entries(void)
{
}

boolean
dirtree_entry_expanded(GNode *dnode)
{
	if (dnode == NULL)
		return FALSE;
	return (DIR_NODE_DESC(dnode)->tnode != NULL) ? TRUE : FALSE;
}

void
dirtree_entry_collapse_recursive(GNode *dnode)
{
	if (dnode == NULL)
		return;
	/* Clears only dnode's own flag -- descendants keep theirs, exactly
	 * like gtk_tree_view_collapse_row( ) / the SDL port */
	set_tree_row_expanded( dnode, FALSE );
}

void
dirtree_entry_expand(GNode *dnode)
{
	if (dnode == NULL)
		return;
	set_tree_row_expanded( dnode, TRUE );
	expand_ancestors( dnode );
}

void
dirtree_entry_expand_recursive(GNode *dnode)
{
	if (dnode == NULL)
		return;
	expand_subtree_recursive( dnode );
	expand_ancestors( dnode );
}
```

Also update the stale claim in the file-header comment ("no-op/headless implementations") with one line noting the dirtree section is stateful and why.

- [ ] **Step 2: Rebuild and run the whole existing suite (regression check on the stub change)**

Run: `export PATH="/opt/homebrew/bin:$PATH" && meson test -C builddir`
Expected: all 4 existing tests PASS (`scanfs`, `nvstore`, `color_persistence`, `fsn_layout`) — none of them read dirtree expansion state, so stateful flags must change nothing. Also confirm `fsv-scan` (the smoke CLI linking the same stubs) still builds: `ninja -C builddir`.

- [ ] **Step 3: Commit**

```bash
git add tools/fsv-headless-stubs.c
git commit -m "test: make the headless dirtree stubs stateful

dirtree_entry_expanded() always returned FALSE, which made camera.c's
collapsed-parent handling (fa9383c) untestable headlessly: after
fsn_ensure_parent_expanded() expands the target's parent, the stub
would still report it collapsed and the DEBUG assert would abort.
Mirror the SDL frontend's real semantics (DirNodeDesc::tnode as a
NULL/non-NULL flag, depth<=2 starts open, collapse clears one flag,
expand opens the ancestor chain) so the upcoming regression test -- and
any future camera/colexp test -- exercises the real contract."
```

---

### Task 2: Headless regression test for both guarded camera entry points

New test `tests/test_fsn_camera.c`, linking exactly like `test_fsn_layout` (libfsvcore objects + headless stubs + fixture). It reproduces TODO.md bug #1's steady-state scenario at the flag level: parent directory flag-collapsed, camera sent to a child that FSN keeps drawn/pickable. With the fix, `fsn_ensure_parent_expanded()` pre-expands; without it, the `g_assert` aborts the test process (DEBUG is on) — so the test fails exactly when the bug exists. Step 4 proves that by temporarily swapping in the pre-fix `camera.c`.

**Files:**
- Create: `tests/test_fsn_camera.c`
- Modify: `tests/meson.build` (append the new executable + test entry)

**Interfaces:**
- Consumes: Task 1's stateful stubs (`dirtree_entry_expanded()`, `dirtree_entry_collapse_recursive()` — collapse clears only the target's own flag); `scanfs()`, `root_dnode`, `globals` (`common.h`/`fsv.h`); `fsn_geometry_init()` (`geometry-fsn.h`); `camera_init()`, `camera_look_at()`, `camera_warp_to()` (`camera.h`).
- Produces: meson test named `fsn_camera` (later tasks/docs refer to it by that name).

- [ ] **Step 1: Write the test**

Create `tests/test_fsn_camera.c`:

```c
/* tests/test_fsn_camera.c
 *
 * SPDX-License-Identifier: MIT
 *
 * Regression lock for the FSN collapsed-parent camera fix (fa9383c,
 * fsn_ensure_parent_expanded( ) in src/camera.c; TODO.md "Known bugs"
 * item 1). FSV_FSN -- unlike every other mode -- keeps a collapsed
 * directory's own file children drawn and pickable on its pedestal, so
 * a plain single click (camera_look_at( )) or a warp double-click
 * (camera_warp_to( )) can legitimately target a node whose parent the
 * dirtree flags as collapsed. Before the fix, that tripped both
 * functions' DEBUG assertion "parent must be expanded" -- a process
 * abort. This test drives both entry points against a flag-collapsed
 * parent; with -DDEBUG live in the default buildtype, the pre-fix code
 * aborts here (g_assert), so the test FAILS iff the bug is back.
 *
 * LINKING. Exactly like test_fsn_layout: libfsvcore objects plus
 * tools/fsv-headless-stubs.c -- whose dirtree stubs are STATEFUL
 * (DirNodeDesc::tnode flag, mirroring src/sdl/ui_panels.cpp); this
 * test is the reason they are. camera.c and colexp.c are both in
 * libfsvcore, so the whole fix path runs for real; the morphs the
 * camera schedules simply never advance (no frame loop), which is fine
 * -- everything asserted here is synchronous. */

#include <assert.h>
#include <string.h>

#include "common.h" /* pulls in glib.h, and must precede it: G_LOG_DOMAIN */
#include "fsv.h"
#include "camera.h"
#include "dirtree.h"
#include "geometry-fsn.h"
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

int
main(void)
{
	GNode *root, *dir_a, *dir_b, *file2;

	scanfs(FIXTURE_DIR); /* FIXTURE_DIR injected by meson (-D) */
	assert(globals.fstree != NULL);

	root = root_dnode;
	assert(root != NULL);
	dir_a = child_named(root, "dir-a");
	assert(dir_a != NULL && NODE_IS_DIR(dir_a));
	dir_b = child_named(dir_a, "dir-b");
	assert(dir_b != NULL && NODE_IS_DIR(dir_b));
	file2 = child_named(dir_a, "file2.bin");
	assert(file2 != NULL && !NODE_IS_DIR(file2));

	/* FSN mode, laid out, camera at initial view -- the state a real
	 * session is in when the user clicks */
	globals.fsv_mode = FSV_FSN;
	fsn_geometry_init(root);
	camera_init(FSV_FSN, TRUE);

	/* The stubs' initial policy already leaves dir-a (depth 3)
	 * collapsed, same as a fresh scan in the real frontends; collapse
	 * explicitly anyway so the precondition is local and survives any
	 * future change to that policy. */
	dirtree_entry_collapse_recursive(dir_a);
	assert(!dirtree_entry_expanded(dir_a));

	/* 1. The exact TODO.md bug: single-click look-at on a file child of
	 * a collapsed directory (FSN draws it on the pedestal regardless).
	 * Pre-fix: g_assert abort inside camera_look_at_full( ). Post-fix:
	 * fsn_ensure_parent_expanded( ) has expanded dir-a before the
	 * assertion runs, and the pan commits normally. */
	camera_look_at(file2);
	assert(dirtree_entry_expanded(dir_a));
	assert(globals.current_node == file2);

	/* 2. Same invariant on the other guarded entry point: a warp
	 * (double-click) landing on a child of a collapsed directory --
	 * reachable mid-collapse-morph, when the child is still drawn. */
	dirtree_entry_collapse_recursive(dir_a);
	assert(!dirtree_entry_expanded(dir_a));
	camera_warp_to(dir_b);
	assert(dirtree_entry_expanded(dir_a));
	assert(globals.current_node == dir_b);

	/* 3. Ancestor CHAIN, not just the immediate parent: collapse both
	 * root and dir-a, then look at the file two levels under the
	 * collapsed root. COLEXP_EXPAND_ANY must reopen the whole chain. */
	dirtree_entry_collapse_recursive(dir_a);
	dirtree_entry_collapse_recursive(root);
	camera_look_at(file2);
	assert(dirtree_entry_expanded(root));
	assert(dirtree_entry_expanded(dir_a));
	assert(globals.current_node == file2);

	return 0;
}
```

- [ ] **Step 2: Register the test in meson**

Append to `tests/meson.build`:

```meson
# Regression lock for the FSN collapsed-parent camera fix (fa9383c,
# fsn_ensure_parent_expanded() -- TODO.md "Known bugs" item 1):
# camera_look_at()/camera_warp_to() on a node whose parent directory is
# flag-collapsed must pre-expand it instead of tripping their DEBUG
# assertion. Depends on the headless dirtree stubs being stateful.
test_fsn_camera = executable('test_fsn_camera',
  ['test_fsn_camera.c', headless_stubs_src],
  objects: libfsvcore.extract_all_objects(recursive: false),
  dependencies: [glibdep, cglm_dep, libm, libmisc_dep, libdebug_dep],
  include_directories: incdir,
  c_args: ['-DFIXTURE_DIR="@0@"'.format(meson.current_source_dir() / 'fixture')])
test('fsn_camera', test_fsn_camera)
```

- [ ] **Step 3: Build and run — must PASS against the fixed code**

Run: `export PATH="/opt/homebrew/bin:$PATH" && meson test -C builddir fsn_camera --print-errorlogs`
Expected: PASS. If it fails on `camera_init()` ordering or an unexpected morph/assert, fix the TEST (setup order, assertions), never `src/camera.c`.

- [ ] **Step 4: Prove the test catches the original bug (TDD-equivalent for an already-landed fix)**

The fix is the only post-`fa9383c` change to `src/camera.c` (verified: `git log --oneline fa9383c..HEAD -- src/camera.c` is empty), so the pre-fix file is `fa9383c~1`'s:

```bash
git checkout fa9383c~1 -- src/camera.c
meson test -C builddir fsn_camera --print-errorlogs
```

Expected: FAIL — the test binary aborts on `camera_look_at_full()`'s `g_assert( dirtree_entry_expanded( node->parent ) )`. Then restore and re-verify:

```bash
git checkout HEAD -- src/camera.c
meson test -C builddir --print-errorlogs
```

Expected: `git status` shows `src/camera.c` clean again, and ALL tests (the 4 pre-existing + `fsn_camera`) PASS.

- [ ] **Step 5: Commit**

```bash
git add tests/test_fsn_camera.c tests/meson.build
git commit -m "test: regression-lock the FSN collapsed-parent camera fix

Headless test for fa9383c (fsn_ensure_parent_expanded()): drives
camera_look_at() at a file child of a flag-collapsed directory and
camera_warp_to() at a subdirectory of one -- FSN keeps both drawn and
pickable while collapsed, the exact single-click / warp scenarios that
aborted DEBUG builds -- plus a two-level collapsed ancestor chain.
With -DDEBUG live in the default buildtype, reverting the fix makes
this test abort at the original assertion (verified by temporarily
checking out fa9383c~1's camera.c), so it fails iff the bug returns."
```

---

### Task 3: Close the TODO.md ticket

**Files:**
- Modify: `TODO.md:9-16` (first "Known bugs" item)

**Interfaces:**
- Consumes: Task 2's meson test name `fsn_camera` and file `tests/test_fsn_camera.c`.
- Produces: nothing downstream — docs only.

- [ ] **Step 1: Mark the ticket resolved**

In `TODO.md`, replace the first "Known bugs" item (lines 9–16, the `- [ ]` block ending with "(PORTING.md, Task C3 gaps)") with a checked, past-tense entry so the closure stays visible in history:

```markdown
- [x] ~~**Clicking a file child of a *collapsed* directory aborts DEBUG builds
  (FSN mode).**~~ **Was already fixed** when this file was first written —
  commit fa9383c (final Milestone C review) added
  `fsn_ensure_parent_expanded()` in `src/camera.c`, centralized ahead of
  the DEBUG assertions in both `camera_look_at_full()` and
  `camera_warp_to()`; this ticket was collected from PORTING.md's earlier
  "disclosed gaps" note without noticing the later fix round. Now
  regression-locked by `tests/test_fsn_camera.c` (meson test
  `fsn_camera`), which also made the headless dirtree stubs stateful.
```

- [ ] **Step 2: Commit**

```bash
git add TODO.md
git commit -m "docs: close TODO's collapsed-click ticket (fixed by fa9383c, now tested)

The first Known-bugs item was stale on arrival: it was collected from
PORTING.md's pre-fix disclosed-gaps notes, but fa9383c had already
fixed it in the same fix round that preceded TODO.md's own commit.
Point it at the fix and at the new fsn_camera regression test."
```

- [ ] **Step 3: Push**

Run: `git push origin fsn-mode`
Expected: fsn-mode updated on origin; CI (macOS Metal + Linux legs) runs the new test — check `gh run watch` or the Actions page for green before calling the task done.

---

## Optional follow-up (not a task — needs a human)

A 30-second manual QA pass in the real app would close the loop end-to-end: `builddir/src/sdl/fsv --fsn <some dir>`, collapse a directory via the context menu or double-click, then single-click one of the file boxes still standing on its pedestal. Expected: the directory reopens and the camera flies to the file — no abort (DEBUG build). The sandbox can't drive ImGui clicks (PORTING.md, test-harness limitations), which is why this stays optional on top of the headless lock.
