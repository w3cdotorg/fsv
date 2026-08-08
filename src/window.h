/* window.h */

/* Main program window */

/* fsv - 3D File System Visualizer
 * Copyright (C)1999 Daniel Richard G. <skunk@mit.edu>
 *
 * SPDX-License-Identifier:  LGPL-2.1-or-later
 */


#ifdef FSV_WINDOW_H
	#error
#endif
#define FSV_WINDOW_H

/* No unconditional <gtk/gtk.h> here: window.h is included by the headless
 * core (camera.c, color.c, scanfs.c) for the GTK-free frontend-notification
 * functions below. GtkApplication-typed decls are gated behind __GTK_H__,
 * same convention as gui.h — callers that need window_init() must include
 * <gtk/gtk.h> themselves before this header. */

typedef enum {
	SB_LEFT,
	SB_RIGHT
} StatusBarID;


#ifdef __GTK_H__
void window_init(GtkApplication *app, gpointer user_data);
#endif
void window_set_access( boolean enabled );
#ifdef FSV_COLOR_H
void window_set_color_mode( ColorMode mode );
#endif
void window_birdseye_view_off( void);
void window_statusbar( StatusBarID sb_id, const char *message );

/* Query the access state most recently pushed by window_set_access( ).
 * SDL frontend only: implemented by src/sdl/stubs.c so src/sdl/
 * ui_rail.cpp can grey out the camera rail's buttons/sliders while the
 * camera is busy (bird's-eye pan, look-at pan), the same condition GTK's
 * window_set_access( ) already applies to its own toolbar/scrollbars via
 * gtk_widget_set_sensitive( ) over sw_widget_list. The GTK frontend never
 * calls this -- its widgets already carry their own sensitivity state. */
boolean window_access_enabled( void );

/* Query/force the bird's-eye-view toggle state the rail's "Birds eye"
 * button displays. SDL frontend only (src/sdl/stubs.c), mirroring GTK's
 * private birdseye_view_tbutton_w: window_birdseye_view_off( ) above is
 * the same "core turned it off on its own" signal GTK's toggle button
 * already reacts to (e.g. camera_look_at_full( ) exiting bird's-eye view
 * when the user picks a new node); window_birdseye_set_active( ) is the
 * "user just turned it on/off via the rail" counterpart to GTK's toggle
 * button's own widget state, called by ui_rail.cpp right before it asks
 * camera_birdseye_view( ) to do the corresponding camera pan. */
boolean window_birdseye_active( void );
void window_birdseye_set_active( boolean active );


/* end window.h */
