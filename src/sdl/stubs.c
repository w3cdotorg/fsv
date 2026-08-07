/* src/sdl/stubs.c — SPDX-License-Identifier: MIT
 *
 * Frontend surfaces the SDL/Metal build does not have yet.
 *
 * The fsv core notifies its frontend through a set of one-way hooks
 * (redraw a widget, update a status bar, label a node). The GTK frontend
 * implements them in dirtree.c/filelist.c/gui.c/window.c/viewport.c/
 * tmaptext.c/about.c; this build implements the ones that already have an
 * SDL counterpart for real, and no-ops the rest until the task that owns
 * them lands:
 *
 *   text_ ...            -> Task 3.4 (tmaptext.c -> text3d.cpp)
 *   dirtree_ / filelist_ -> Task 5.2 (ImGui panels)
 *   window_ / gui_update -> Task 5.1/5.3 (ImGui menu bar, dialogs)
 *   about ...            -> no About/splash presentation in this frontend
 *   viewport_ ...        -> Task 4.2 (picking owns the node table)
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
#include "tmaptext.h"
#include "viewport.h"
#include "window.h"

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

/* viewport.h */
void
viewport_pass_node_table(GNode **new_node_table, size_t nz)
{
	/* The GTK implementation (viewport.c) takes ownership and frees the
	 * previous table; picking (Task 4.2) is what will need it here. */
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

/* tmaptext.h — the texture-mapped text engine is Task 3.4 */
void
text_init(void)
{
}

void
text_pre(void)
{
}

void
text_post(void)
{
}

void
text_draw_straight(const char *text, const XYZvec *text_pos,
    const XYvec *text_max_dims)
{
	(void)text;
	(void)text_pos;
	(void)text_max_dims;
}

void
text_draw_straight_rotated(const char *text, const RTZvec *text_pos,
    const XYvec *text_max_dims)
{
	(void)text;
	(void)text_pos;
	(void)text_max_dims;
}

void
text_draw_curved(const char *text, const RTZvec *text_pos,
    const RTvec *text_max_dims)
{
	(void)text;
	(void)text_pos;
	(void)text_max_dims;
}

void
text_set_color(float red, float green, float blue)
{
	(void)red;
	(void)green;
	(void)blue;
}

void
text_upload_mvp(float *mvp)
{
	(void)mvp;
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
