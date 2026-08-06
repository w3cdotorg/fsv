/* window.c */

/* Main window definition */

/* fsv - 3D File System Visualizer
 * Copyright (C)1999 Daniel Richard G. <skunk@mit.edu>
 * Copyright (C) 2021 Janne Blomqvist <blomqvist.janne@gmail.com>
 *
 * SPDX-License-Identifier:  LGPL-2.1-or-later
 */


#include "common.h"
#include "window.h"

#include <gtk/gtk.h>

#include "about.h"
#include "callbacks.h"
#include "camera.h"
#include "color.h"
#include "dialog.h"
#include "dirtree.h"
#include "filelist.h"
#include "fsv.h"
#include "fsv-platform.h"
#include "gui.h"
#include "ogl.h" /* ogl_draw( ) */
#include "viewport.h"

/* Toolbar button icons */
#include "xmaps/back.xpm"
#include "xmaps/cd-root.xpm"
#include "xmaps/cd-up.xpm"
#include "xmaps/birdseye_view.xpm"

/* Main program icon */
#include "xmaps/fsv-icon.xpm"


/* Color radio menu items */
static GtkWidget *color_by_nodetype_rmenu_item_w;
static GtkWidget *color_by_timestamp_rmenu_item_w;
static GtkWidget *color_by_wpattern_rmenu_item_w;

/* Bird's-eye view button (on toolbar) */
static GtkWidget *birdseye_view_tbutton_w;

/* List of widgets that can be enabled or disabled on the fly using
 * window_set_access( ) */
static GList *sw_widget_list = NULL;

/* Left and right statusbar widgets */
static GtkWidget *left_statusbar_w;
static GtkWidget *right_statusbar_w;

/* Widgets backing the fsv_platform hook implementations below */
static GtkWidget *hook_gl_area_w;
static GtkWidget *hook_x_scrollbar_w;
static GtkWidget *hook_y_scrollbar_w;


/* fsv_platform.request_frame: schedule an animation tick as a low-priority
 * GTK idle callback, exactly as core code used to do directly */
static gboolean
gtk_tick_cb( gpointer data )
{
	return fsv_animation_tick( );
}

static void
gtk_request_frame( void )
{
	g_idle_add_full( G_PRIORITY_LOW, gtk_tick_cb, NULL, NULL );
}


/* fsv_platform.render_frame: render the viewport now.
 * Note: named fsv_gtk_render_frame( ), not gtk_render_frame( ), to avoid
 * clashing with GTK's own gtk_render_frame( ) (see gtk/gtkrender.h) */
static void
fsv_gtk_render_frame( void )
{
	ogl_draw( );
}


/* fsv_platform.viewport_size: current GL area allocation, in pixels */
static void
gtk_viewport_size( int *width, int *height )
{
	GtkAllocation allocation;

	gtk_widget_get_allocation( hook_gl_area_w, &allocation );
	*width = allocation.width;
	*height = allocation.height;
}


/* Returns the GtkAdjustment backing the given scroll axis */
static GtkAdjustment *
hook_scrollbar_adjustment( int axis )
{
	return gtk_range_get_adjustment( GTK_RANGE(axis == 0 ? hook_x_scrollbar_w : hook_y_scrollbar_w) );
}


/* fsv_platform.set_scroll: update the scrollbar adjustment for the given
 * axis (0=x, 1=y) */
static void
gtk_set_scroll( int axis, double lower, double upper, double page, double pos )
{
	GtkAdjustment *adj = hook_scrollbar_adjustment( axis );

	gtk_adjustment_set_lower( adj, lower );
	gtk_adjustment_set_upper( adj, upper );
	gtk_adjustment_set_page_size( adj, page );
	gtk_adjustment_set_value( adj, pos );
}


/* fsv_platform.get_scroll: current value of the scrollbar adjustment for
 * the given axis (0=x, 1=y) */
static double
gtk_get_scroll( int axis )
{
	return gtk_adjustment_get_value( hook_scrollbar_adjustment( axis ) );
}


/* Constructs the main program window. The specified mode will be the one
 * initially selected in the Vis menu */
void
window_init(GtkApplication *app, gpointer user_data)
{
	GtkWidget *main_window_w;
	GtkWidget *main_vbox_w;
	GtkWidget *menu_bar_w;
	GtkWidget *menu_w;
	GtkWidget *menu_item_w;
	GtkWidget *hpaned_w;
	GtkWidget *vpaned_w;
	GtkWidget *left_vbox_w;
	GtkWidget *right_vbox_w;
	GtkWidget *hbox_w;
	GtkWidget *button_w;
	GtkWidget *frame_w;
	GtkWidget *dir_tree_w;
	GtkWidget *file_list_w;
        GtkWidget *gl_area_w;
	GtkWidget *x_scrollbar_w;
	GtkWidget *y_scrollbar_w;
	int window_width, window_height;

	Fsv_init_data* fid = (Fsv_init_data*)user_data;
	FsvMode fsv_mode = fid->mode;
	/* Main window widget */
	main_window_w = gtk_application_window_new(app);
	gtk_window_set_title( GTK_WINDOW(main_window_w), "fsv" );
	gtk_window_set_resizable(GTK_WINDOW(main_window_w), TRUE);
	window_width = 1920 / 2;
	window_height = 2584 * window_width / 4181;
	gtk_widget_set_size_request(main_window_w, window_width, window_height);

	/* Main vertical box widget */
	main_vbox_w = gui_vbox_add( main_window_w, 0 );

	/* Build menu bar */

	/* Menu bar widget */
	menu_bar_w = gtk_menu_bar_new( );
	gtk_box_pack_start( GTK_BOX(main_vbox_w), menu_bar_w, FALSE, FALSE, 0 );
	gtk_widget_show( menu_bar_w );

	/* File menu */
	menu_w = gui_menu_add( menu_bar_w, _("File") );
	/* File menu items */
	menu_item_w = gui_menu_item_add( menu_w, _("Change root..."), on_file_change_root_activate, NULL );
	gui_keybind( menu_item_w, _("^N") );
	G_LIST_APPEND(sw_widget_list, menu_item_w);
#if 0
	gui_menu_item_add( menu_w, _("Save settings"), on_file_save_settings_activate, NULL );
#endif
	gui_separator_add( menu_w );
	menu_item_w = gui_menu_item_add( menu_w, _("Exit"), on_file_exit_activate, NULL );
	gui_keybind( menu_item_w, _("^Q") );

	/* Vis menu */
	menu_w = gui_menu_add( menu_bar_w, _("Vis") );
	/* Vis menu items */
	gui_radio_menu_begin( fsv_mode -1 );
#if 0 /* DiscV mode needs more work */
	gui_radio_menu_item_add( menu_w, _("DiscV"), on_vis_discv_activate, NULL );
/* Note: don't forget to remove the "-1" three lines up */
#endif
	gui_radio_menu_item_add( menu_w, _("MapV"), on_vis_mapv_activate, NULL );
	gui_radio_menu_item_add( menu_w, _("TreeV"), on_vis_treev_activate, NULL );

	/* Color menu */
	menu_w = gui_menu_add( menu_bar_w, _("Colors") );
	/* Color menu items */
	gui_radio_menu_begin( 0 );
	menu_item_w = gui_radio_menu_item_add( menu_w, _("By node type"), on_color_by_nodetype_activate, NULL );
	G_LIST_APPEND(sw_widget_list, menu_item_w);
	color_by_nodetype_rmenu_item_w = menu_item_w;
	menu_item_w = gui_radio_menu_item_add( menu_w, _("By timestamp"), on_color_by_timestamp_activate, NULL );
	G_LIST_APPEND(sw_widget_list, menu_item_w);
	color_by_timestamp_rmenu_item_w = menu_item_w;
	menu_item_w = gui_radio_menu_item_add( menu_w, _("By wildcards"), on_color_by_wildcards_activate, NULL );
	G_LIST_APPEND(sw_widget_list, menu_item_w);
	color_by_wpattern_rmenu_item_w = menu_item_w;
	gui_separator_add( menu_w );
	gui_menu_item_add( menu_w, _("Setup..."), on_color_setup_activate, NULL );

#ifdef DEBUG
	/* Debug menu */
	menu_w = gui_menu_add( menu_bar_w, "Debug" );
	/* Debug menu items */
	gui_menu_item_add( menu_w, "Memory totals", debug_show_mem_totals, NULL );
	gui_menu_item_add( menu_w, "Memory summary", debug_show_mem_summary, NULL );
	gui_menu_item_add( menu_w, "Memory stats", debug_show_mem_stats, NULL );
	gui_separator_add( menu_w );
#endif

	/* Help menu (right-justified) */
	menu_w = gui_menu_add( menu_bar_w, _("Help") );
	/* Help menu items */
	gui_menu_item_add( menu_w, _("Contents..."), on_help_contents_activate, NULL );
	gui_separator_add( menu_w );
	gui_menu_item_add( menu_w, _("About fsv..."), on_help_about_fsv_activate, NULL );

	/* Done with the menu bar */

	/* Main horizontal paned widget */
	hpaned_w = gui_hpaned_add( main_vbox_w, window_width / 5 );

	/* Vertical box for everything in the left pane */
	left_vbox_w = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_paned_add1( GTK_PANED(hpaned_w), left_vbox_w );
	gtk_widget_show( left_vbox_w );

	/* Horizontal box for toolbar buttons */
	hbox_w = gui_hbox_add( left_vbox_w, 2 );

	/* "back" button */
	button_w = gui_button_add( hbox_w, NULL, on_back_button_clicked, NULL );
	gui_pixbuf_xpm_add(button_w, back_xpm);
	G_LIST_APPEND(sw_widget_list, button_w);
	/* "cd /" button */
	button_w = gui_button_add( hbox_w, NULL, on_cd_root_button_clicked, NULL );
	gui_pixbuf_xpm_add(button_w, cd_root_xpm);
	G_LIST_APPEND(sw_widget_list, button_w);
	/* "cd .." button */
	button_w = gui_button_add( hbox_w, NULL, on_cd_up_button_clicked, NULL );
	gui_pixbuf_xpm_add(button_w, cd_up_xpm);
	G_LIST_APPEND(sw_widget_list, button_w);
	/* "bird's-eye view" toggle button */
	button_w = gui_toggle_button_add( hbox_w, NULL, FALSE, on_birdseye_view_togglebutton_toggled, NULL );
	gui_pixbuf_xpm_add(button_w, birdseye_view_xpm);
	G_LIST_APPEND(sw_widget_list, button_w);
	birdseye_view_tbutton_w = button_w;

	/* Frame to encase the directory tree / file list */
	frame_w = gui_frame_add( left_vbox_w, NULL );

	/* Vertical paned widget for directory tree / file list */
	vpaned_w = gui_vpaned_add( frame_w, window_height / 3 );

	/* Directory tree goes in top pane */
	dir_tree_w = gui_tree_add( NULL );
	gtk_paned_add1(GTK_PANED(vpaned_w), gtk_widget_get_parent(dir_tree_w));
	gtk_widget_show(gtk_widget_get_parent(dir_tree_w));

	/* File list goes in bottom pane */
	file_list_w = gui_filelist_new(NULL);
	gtk_paned_add2(GTK_PANED(vpaned_w), gtk_widget_get_parent(file_list_w));
	gtk_widget_show(gtk_widget_get_parent(file_list_w));

	/* Left statusbar */
	left_statusbar_w = gui_statusbar_add( left_vbox_w );

	/* Vertical box for everything in the right pane */
	right_vbox_w = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_paned_add2( GTK_PANED(hpaned_w), right_vbox_w );
	gtk_widget_show( right_vbox_w );

	/* Horizontal box for viewport and y-scrollbar */
	hbox_w = gui_hbox_add( right_vbox_w, 0 );
	gui_widget_packing( hbox_w, EXPAND, FILL, AT_START );

	/* Main viewport (OpenGL area widget) */
	gl_area_w = gui_gl_area_add( hbox_w );
	g_signal_connect(G_OBJECT(gl_area_w), "event", G_CALLBACK(viewport_cb), NULL);

	/* y-scrollbar */
	y_scrollbar_w = gui_vscrollbar_add( hbox_w, NULL );
	G_LIST_APPEND(sw_widget_list, y_scrollbar_w);
	/* x-scrollbar */
	x_scrollbar_w = gui_hscrollbar_add( right_vbox_w, NULL );
	G_LIST_APPEND(sw_widget_list, x_scrollbar_w);

	/* Right statusbar */
	right_statusbar_w = gui_statusbar_add( right_vbox_w );

	/* Bind program icon to main window */
	gui_window_icon_xpm( main_window_w, fsv_icon_xpm );

	/* Attach keybindings */
	gui_keybind( main_window_w, NULL );

	/* Send out the widgets to their respective modules */
	dialog_pass_main_window_widget( main_window_w );
	dirtree_pass_widget( dir_tree_w );
	filelist_pass_widget( file_list_w );
	camera_pass_scrollbar_widgets( x_scrollbar_w, y_scrollbar_w );

	/* Install the GTK platform hooks. Must happen before anything
	 * that can call redraw( ) (e.g. fsv_load( ) below) */
	hook_gl_area_w = gl_area_w;
	hook_x_scrollbar_w = x_scrollbar_w;
	hook_y_scrollbar_w = y_scrollbar_w;
	fsv_platform.request_frame = gtk_request_frame;
	fsv_platform.render_frame = fsv_gtk_render_frame;
	fsv_platform.viewport_size = gtk_viewport_size;
	fsv_platform.set_scroll = gtk_set_scroll;
	fsv_platform.get_scroll = gtk_get_scroll;

	/* Showtime! */
	gtk_widget_show( main_window_w );

	color_init();
	fsv_load(fid->root_dir);
	xfree(fid->root_dir);
}


/* This enables/disables the switchable widgets */
void
window_set_access( boolean enabled )
{
	GtkWidget *widget;
	GList *llink;

	llink = sw_widget_list;
	while (llink != NULL) {
		widget = (GtkWidget *)llink->data;

		gtk_widget_set_sensitive( widget, enabled );

		llink = llink->next;
	}
}


/* Resets the Color radio menu to the given mode */
void
window_set_color_mode( ColorMode mode )
{
	GtkWidget *rmenu_item_w;
	GCallback handler;

	switch (mode) {
		case COLOR_BY_NODETYPE:
		rmenu_item_w = color_by_nodetype_rmenu_item_w;
		handler = G_CALLBACK(on_color_by_nodetype_activate);
		break;

		case COLOR_BY_TIMESTAMP:
		rmenu_item_w = color_by_timestamp_rmenu_item_w;
		handler = G_CALLBACK(on_color_by_timestamp_activate);
		break;

		case COLOR_BY_WPATTERN:
		rmenu_item_w = color_by_wpattern_rmenu_item_w;
		handler = G_CALLBACK(on_color_by_wildcards_activate);
		break;

		SWITCH_FAIL
	}

	g_signal_handlers_block_by_func(G_OBJECT(rmenu_item_w), handler, NULL );
	gtk_check_menu_item_set_active( GTK_CHECK_MENU_ITEM(rmenu_item_w), TRUE );
	g_signal_handlers_unblock_by_func(G_OBJECT(rmenu_item_w), handler, NULL );
}


/* Pops out the bird's-eye view toggle button
 * Note: This should only be called from camera.c, as the bird's-eye-view
 * mode flag (local to that module) must be updated in tandem */
void
window_birdseye_view_off( void )
{
	g_signal_handlers_block_by_func(G_OBJECT(birdseye_view_tbutton_w), G_CALLBACK(on_birdseye_view_togglebutton_toggled), NULL);
	gtk_toggle_button_set_active( GTK_TOGGLE_BUTTON(birdseye_view_tbutton_w), FALSE );
	g_signal_handlers_unblock_by_func(G_OBJECT(birdseye_view_tbutton_w), G_CALLBACK(on_birdseye_view_togglebutton_toggled), NULL);
}


/* Displays a message in one of the statusbars */
void
window_statusbar( StatusBarID sb_id, const char *message )
{
	switch (sb_id) {
		case SB_LEFT:
		gui_statusbar_message( left_statusbar_w, message );
		break;

		case SB_RIGHT:
		gui_statusbar_message( right_statusbar_w, message );
		break;

		SWITCH_FAIL
	}
}


/* end window.c */
