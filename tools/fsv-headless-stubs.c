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
 * This shim provides no-op/headless implementations of exactly the
 * symbols the smoke CLI (fsv-scan) and the fixture unit test need to
 * link, so libfsvcore can be exercised standalone without pulling in
 * any GTK/GL frontend code. It is NOT part of libfsvcore itself — real
 * frontends (GTK today, SDL/Metal later) provide their own real
 * implementations of these functions instead of linking this file.
 */

#include "common.h"
#include "color.h"
#include "dirtree.h"
#include "filelist.h"
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

/* gui.h */
void
gui_update(void)
{
}

/* dirtree.h */
void
dirtree_clear(void)
{
}

void
dirtree_entry_new(GNode *dnode)
{
	(void)dnode;
}

void
dirtree_no_more_entries(void)
{
}

boolean
dirtree_entry_expanded(GNode *dnode)
{
	(void)dnode;
	return FALSE;
}

void
dirtree_entry_collapse_recursive(GNode *dnode)
{
	(void)dnode;
}

void
dirtree_entry_expand(GNode *dnode)
{
	(void)dnode;
}

void
dirtree_entry_expand_recursive(GNode *dnode)
{
	(void)dnode;
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

/* Exported by geometry.c since fsn-mode Task B1 so that geometry-fsn.c
 * shares one select-pass color encoding rather than keeping a second
 * copy. tests/test_fsn_layout.c compiles geometry-fsn.c in directly (it
 * is frontend-side, so libfsvcore does not carry it) and therefore needs
 * this to link, even though its layout pass never draws anything. */
void
geometry_node_set_color(GNode *node)
{
	(void)node;
}
