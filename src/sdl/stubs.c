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
 *   dirtree_ / filelist_ -> Task 5.2, src/sdl/ui_panels.cpp (real now --
 *                           NOT here; see that file)
 *   window_ ...          -> Task 5.1/5.3 (ImGui menu bar, dialogs)
 *   about ...            -> no About/splash presentation in this frontend
 *   viewport_ ...        -> the node table lives here since Task 3.3;
 *                           Task 4.1's src/sdl/input.cpp reads it back
 *                           through viewport_node_for_id( ) below, and
 *                           Task 4.2 is what makes gpu_pick( ) (the id
 *                           this looks up) return anything but 0
 *
 * tmaptext.c's text_*() entry points are NOT here as of Task 3.4: this
 * build compiles src/tmaptext.c for real (see src/sdl/meson.build),
 * exactly like geometry.c.
 *
 * This is deliberately separate from tools/fsv-headless-stubs.c, which
 * fsv-scan and test_scanfs use: that one also stubs out geometry.c, which
 * this build compiles for real.
 */

#include "common.h"

#include "about.h"
#include "color.h"
#include "gui.h"
#include "viewport.h"
#include "window.h"

/* gui.h's gui_update() and window.h's window_statusbar() are NOT here:
 * src/sdl/main.cpp implements both for real. scanfs() blocks the only
 * thread for the whole scan and calls gui_update() per directory entry to
 * keep the frontend alive, so it has to pump SDL events and render -- and
 * the scan's progress text arrives through window_statusbar(). */

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

/* Node-id -> GNode * lookup for src/sdl/input.cpp's node_at_cursor( )
 * (Task 4.1). Same bounds check and warning as viewport.c's
 * node_at_location( ); id 0 ("nothing there") is not looked up at all. */
GNode *
viewport_node_for_id(unsigned int id)
{
	if (id == 0)
		return NULL;
	if (id >= node_table_size) {
		g_warning("Got node id %u larger than node table size %zu\n", id, node_table_size);
		return NULL;
	}
	return node_table[id];
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
