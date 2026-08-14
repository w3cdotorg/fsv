/* tools/fsv-headless-stubs.c
 *
 * SPDX-License-Identifier: MIT
 *
 * libfsvcore (scanfs.c, colexp.c, color.c, common.c, animation.c, camera.c)
 * calls into a handful of frontend-notification functions that are
 * normally implemented by dirtree.c, filelist.c, geometry.c, gui.c,
 * viewport.c and window.c. Those files are GTK/GL-bound and deliberately
 * NOT part of libfsvcore (geometry.c joins in M3 once its GL calls are
 * gone; the others are permanently frontend-side).
 *
 * This shim provides headless implementations of exactly the symbols
 * the smoke CLI (fsv-scan) and the fixture unit tests need to link.
 * Most are no-ops; the dirtree section below is deliberately stateful
 * (see its own comment). This shim is not part of libfsvcore -- real
 * frontends (GTK today, SDL/Metal) provide their own real
 * implementations of these functions instead of linking this file.
 */

#include "common.h"
#include "color.h"
#include "dirtree.h"
#include "filelist.h"
#include "fsv-platform.h"
#include "geometry.h"
#include "gpu.h" /* gpu_set_landscape( ) -- see color.c's landscape_set()/_init() */
#include "gui.h"
#include "viewport.h"
#include "window.h"

/* gpu.h -- color.c (Task A1, fsn-mode) calls this the same way it calls
 * window_set_color_mode() below: a frontend-notification hook, now with
 * a real implementation in both src/sdl/gpu.cpp and
 * src/ogl-gpu-compat.c, neither of which is linked into this headless
 * build (see the file header above). */
void
gpu_set_landscape(int index)
{
	(void)index;
}

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

/* gui.h */
void
gui_update(void)
{
}

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

/* filelist.h */
void
filelist_reset_access(void)
{
}

void
filelist_scan_monitor_init(void)
{
}

void
filelist_scan_monitor(int *node_counts, int64 *size_counts)
{
	(void)node_counts;
	(void)size_counts;
}

void
filelist_show_entry(GNode *node)
{
	(void)node;
}

/* viewport.h */
void
viewport_pass_node_table(GNode **new_node_table, size_t nz)
{
	/* The real implementation (viewport.c) takes ownership of
	 * new_node_table and frees it on the next call/at shutdown. This
	 * stub intentionally drops it instead — a deliberate one-shot leak,
	 * acceptable for fsv-scan/test_scanfs's short-lived process lifetime. */
	(void)new_node_table;
	(void)nz;
}

/* window.h */
void
window_set_access(boolean enabled)
{
	(void)enabled;
}

void
window_set_color_mode(ColorMode mode)
{
	(void)mode;
}

void
window_birdseye_view_off(void)
{
}

void
window_statusbar(StatusBarID sb_id, const char *message)
{
	(void)sb_id;
	(void)message;
}

/* geometry.h */
XYvec *
geometry_discv_node_pos(GNode *node)
{
	(void)node;
	return NULL;
}

double
geometry_mapv_node_z0(GNode *node)
{
	(void)node;
	return 0.0;
}

double
geometry_mapv_max_expanded_height(GNode *dnode)
{
	(void)dnode;
	return 0.0;
}

boolean
geometry_treev_is_leaf(GNode *node)
{
	(void)node;
	return TRUE;
}

double
geometry_treev_platform_r0(GNode *dnode)
{
	(void)dnode;
	return 0.0;
}

double
geometry_treev_platform_theta(GNode *dnode)
{
	(void)dnode;
	return 0.0;
}

double
geometry_treev_max_leaf_height(GNode *dnode)
{
	(void)dnode;
	return 0.0;
}

void
geometry_treev_get_extents(GNode *dnode, RTvec *ext_c0, RTvec *ext_c1)
{
	(void)dnode;
	if (ext_c0 != NULL) {
		ext_c0->r = 0.0;
		ext_c0->theta = 0.0;
	}
	if (ext_c1 != NULL) {
		ext_c1->r = 0.0;
		ext_c1->theta = 0.0;
	}
}

void
geometry_queue_rebuild(GNode *dnode)
{
	(void)dnode;
}

void
geometry_camera_pan_finished(void)
{
}

void
geometry_colexp_initiated(GNode *dnode)
{
	(void)dnode;
}

void
geometry_colexp_in_progress(GNode *dnode)
{
	(void)dnode;
}

void
geometry_free_recursive(GNode *dnode)
{
	(void)dnode;
}
