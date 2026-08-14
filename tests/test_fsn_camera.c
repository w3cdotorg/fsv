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
#include "fsv-platform.h"
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

/* fsv_platform (declared in fsv-platform.h, defined zero-initialized in
 * src/animation.c) is the frontend-services hook table libfsvcore calls
 * into -- camera.c's redraw( )/camera_update_scrollbars( ) path calls
 * request_frame( ) and set_scroll( ) unconditionally, in every mode,
 * including FSV_FSN. "Every field must be non-NULL after frontend
 * init" (the header's own comment) is a real precondition, normally
 * met by src/window.c (GTK) or the SDL frontend before any camera call;
 * this test plays that role with plain no-ops, since nothing here drives
 * an actual frame loop or scrollbar widget. */
static void
noop_request_frame(void)
{
}

static void
noop_render_frame(void)
{
}

static void
noop_viewport_size(int *width, int *height)
{
	if (width != NULL)
		*width = 800;
	if (height != NULL)
		*height = 600;
}

static void
noop_set_scroll(int axis, double lower, double upper, double page, double pos)
{
	(void)axis;
	(void)lower;
	(void)upper;
	(void)page;
	(void)pos;
}

static double
noop_get_scroll(int axis)
{
	(void)axis;
	return 0.0;
}

int
main(void)
{
	GNode *root, *dir_a, *dir_b, *file2;

	fsv_platform.request_frame = noop_request_frame;
	fsv_platform.render_frame = noop_render_frame;
	fsv_platform.viewport_size = noop_viewport_size;
	fsv_platform.set_scroll = noop_set_scroll;
	fsv_platform.get_scroll = noop_get_scroll;

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
	 * collapsed root. COLEXP_EXPAND_ANY must reopen the whole chain --
	 * though the stubs' own dirtree_entry_expand( ) already opens root
	 * via expand_ancestors( ) before colexp's own parent recursion even
	 * runs, so what this scenario actually pins is the end-state
	 * invariant (the whole chain left flagged open), not colexp's
	 * recursion path specifically. */
	dirtree_entry_collapse_recursive(dir_a);
	dirtree_entry_collapse_recursive(root);
	assert(!dirtree_entry_expanded(dir_a));
	/* root is the one node the depth<=2 policy starts expanded --
	 * nothing else proves its flag can clear */
	assert(!dirtree_entry_expanded(root));
	camera_look_at(file2);
	assert(dirtree_entry_expanded(root));
	assert(dirtree_entry_expanded(dir_a));
	assert(globals.current_node == file2);

	return 0;
}
