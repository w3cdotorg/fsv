/* src/sdl/stubs.c — SPDX-License-Identifier: MIT
 *
 * Frontend surfaces the SDL/Metal build does not have yet.
 *
 * The fsv core notifies its frontend through a set of one-way hooks
 * (redraw a widget, update a status bar, label a node). The GTK frontend
 * implements them in dirtree.c/filelist.c/gui.c/window.c/viewport.c/
 * about.c; this build implements the ones that already have an SDL
 * counterpart for real, and no-ops the rest until the task that owns
 * them lands:
 *
 *   dirtree_ / filelist_ -> Task 5.2 (ImGui panels)
 *   window_ ...          -> Task 5.1/5.3 (ImGui menu bar, dialogs)
 *   about ...            -> no About/splash presentation in this frontend
 *   viewport_ ...        -> Task 4.2 (picking owns the node table)
 *
 * tmaptext.c's text_*() entry points are NOT here as of Task 3.4: this
 * build compiles src/tmaptext.c for real (see src/sdl/meson.build),
 * exactly like geometry.c.
 *
 * This is deliberately separate from tools/fsv-headless-stubs.c, which
 * fsv-scan and test_scanfs use: that one also stubs out geometry.c, which
 * this build compiles for real.
 *
 * dirtree_entry_expanded() is the one stub whose return value the core
 * actually reads -- geometry.c asks it whether a directory is open.
 * Answering "yes" for every directory would lay out the entire tree at
 * once; answering it only for the root matches the GTK frontend's state
 * on a fresh scan, where dirtree.c expands the root and everything below
 * it starts collapsed.
 */

#include "common.h"

#include "about.h"
#include "color.h"
#include "dirtree.h"
#include "filelist.h"
#include "gui.h"
#include "viewport.h"
#include "window.h"

/* gui.h's gui_update() and window.h's window_statusbar() are NOT here:
 * src/sdl/main.cpp implements both for real. scanfs() blocks the only
 * thread for the whole scan and calls gui_update() per directory entry to
 * keep the frontend alive, so it has to pump SDL events and render -- and
 * the scan's progress text arrives through window_statusbar(). */

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
	return dnode == root_dnode;
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

/* viewport.h
 *
 * Not a no-op: scanfs.c (scanfs.c:361) hands over a NEW_ARRAY that the
 * callee owns, so dropping the pointer would leak the whole table on every
 * rescan. This mirrors what GTK's viewport.c:43-49 does -- free the
 * previous table, store the new one -- and keeps it stored rather than
 * freeing it immediately because picking (Task 4.2) is what reads it: the
 * table maps a color id back to a GNode *. */
static GNode **node_table = NULL;
static size_t node_table_size = 0;

void
viewport_pass_node_table(GNode **new_node_table, size_t nz)
{
	if (node_table != NULL)
		xfree(node_table);
	node_table = new_node_table;
	node_table_size = nz;
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

/* about.h — no About presentation or splash screen in this frontend */
boolean
about(AboutMesg mesg)
{
	(void)mesg;
	return FALSE;
}

void
about_splash_draw(void)
{
}
