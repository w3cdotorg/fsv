/* viewport.h */

/* Viewport routines */

/* fsv - 3D File System Visualizer
 * Copyright (C)1999 Daniel Richard G. <skunk@mit.edu>
 *
 * SPDX-License-Identifier:  LGPL-2.1-or-later
 */


#ifdef FSV_VIEWPORT_H
	#error
#endif
#define FSV_VIEWPORT_H


void viewport_pass_node_table(GNode **new_node_table, size_t nz);
#ifdef __GTK_H__
gboolean viewport_cb( GtkWidget *gl_area_w, GdkEvent *event, gpointer user_data );
#endif
/* Resolves a gpu_pick( )-returned node id back to the GNode * it names,
 * via the table scanfs( ) handed to viewport_pass_node_table( ). Returns
 * NULL for id 0 ("nothing there", gpu_pick( )'s own convention) or an id
 * past the end of the table (warns, mirroring viewport.c's own
 * node_at_location( )). SDL frontend only: implemented by
 * src/sdl/stubs.c, which is where the node table itself already lived
 * (Task 3.3). The GTK frontend keeps its node_table entirely private to
 * viewport.c and never calls this. */
GNode *viewport_node_for_id(unsigned int id);


/* end viewport.h */
