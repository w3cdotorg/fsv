/* camera.c */

/* Camera control */

/* fsv - 3D File System Visualizer
 * Copyright (C)1999 Daniel Richard G. <skunk@mit.edu>
 * Copyright (C) 2021 Janne Blomqvist <blomqvist.janne@gmail.com>
 *
 * SPDX-License-Identifier:  LGPL-2.1-or-later
 */


#include "common.h"
#include "camera.h"

#include "animation.h"
#include "colexp.h" /* colexp(), COLEXP_EXPAND_ANY -- see fsn_ensure_parent_expanded( ) */
#include "dirtree.h" /* dirtree_entry_expanded( ) */
#include "filelist.h"
#include "fsv-platform.h"
#include "geometry.h"
#include "fsn-style.h" /* FSN_GENERATION_GAP (FSV_FSN framing), FSN_FLIGHT_* (flight navigation) */
#include "geometry-fsn.h" /* FSV_FSN layout accessors */
#include "window.h"


/* Lower/upper bounds on pan times (in seconds) */
#define DISCV_CAMERA_MIN_PAN_TIME	0.5
#define DISCV_CAMERA_MAX_PAN_TIME	3.0
#define MAPV_CAMERA_MIN_PAN_TIME	0.5
#define MAPV_CAMERA_MAX_PAN_TIME	4.0
#define TREEV_CAMERA_MIN_PAN_TIME	1.0
#define TREEV_CAMERA_MAX_PAN_TIME	4.0
#define FSN_CAMERA_MIN_PAN_TIME		0.5
#define FSN_CAMERA_MAX_PAN_TIME		4.0

/* FSV_FSN camera, Task B1 PLACEHOLDER.
 *
 * FSN reuses MapV's *camera storage* -- a Cartesian XYZ target plus the
 * base Camera's theta/phi/distance, i.e. MAPV_CAMERA(camera) and the
 * FSV_MAPV arm of both frontends' setup_modelview_matrix( ). What it
 * does NOT reuse is MapV's camera *math*: every mapv_* helper below
 * reads MAPV_GEOM_PARAMS, which in FSN mode holds an FsnPedestal instead
 * (the two modes share NodeDesc::geomparams), so delegating outright
 * would feed the camera another mode's numbers reinterpreted as its own.
 * The fsn_* helpers below are therefore thin equivalents that read the
 * FSN layout (src/geometry-fsn.h) and are otherwise shaped exactly like
 * their MapV counterparts.
 *
 * Task B2 replaces all of this with the real fsn flight model
 * (approach-deceleration, ground-level travel). Until then these values
 * only have to produce a sane, non-degenerate view. */
#define FSN_CAMERA_PHI			15.0	/* low, near-ground pitch, as in
						 * the reference screenshot */
#define FSN_CAMERA_THETA		270.0	/* looking along +y, the
						 * direction the tree grows */

#define TREEV_CAMERA_AVG_VELOCITY	1024.0


/* Used in scrollbar routines */
enum {
	X_AXIS,
	Y_AXIS
};


/* The camera */
static union AnyCamera the_camera;

/* More convenient pointer to the camera */
Camera *camera = CAMERA(&the_camera);

/* Scroll state for one axis of viewport scrolling: lower/upper bound,
 * visible page size, and current position. Computed here and pushed to
 * (or read back from) the frontend's scrollbar widgets via
 * fsv_platform.set_scroll( ) / fsv_platform.get_scroll( ). */
typedef struct {
	double lower;
	double upper;
	double page;
	double value;
} ScrollState;

/* Current viewport scroll state, indexed by X_AXIS/Y_AXIS */
static ScrollState scroll_state[2];

/* Scroll state at outset of a camera pan (for interpolation) */
static ScrollState prev_scroll_state[2];

/* TRUE if the camera is currently moving */
static boolean camera_currently_moving = FALSE;

/* Camera state prior to entering bird's-eye-view mode */
static union AnyCamera pre_birdseye_view_camera;

/* TRUE if in bird's-eye-view mode */
static boolean birdseye_view_active = FALSE;


/* External interface to check if camera is in motion */
boolean
camera_moving( void )
{
	return camera_currently_moving;
}


/* Returns the diameter of a camera's visible range (centered at the
 * target) given the specified field of view and distance to target */
static double
field_diameter( double fov, double distance )
{
	return (2.0 * distance * tan( RAD(0.5 * fov) ));
}


/* Returns the distance that a camera with the given field of view must
 * have to a target object of the specified diameter, if the target object
 * is to fill the field of view (inverse of field_diameter( )) */
static double
field_distance( double fov, double diameter )
{
	return (diameter * (0.5 / tan( RAD(0.5 * fov) )));
}


/* Initializes camera state for the specified mode. initial_view flag
 * specifies whether camera is being initialized for the first time for
 * a particular filesystem (FALSE e.g. after switching the vis mode) */
void
camera_init( FsvMode mode, boolean initial_view )
{
	const FsnPedestal *fsn_ped;
	XYZvec fsn_ext;
	RTvec ext_c1;
	double d, d1, d2;

	/* A mode switch, a rescan or a Reset re-poses the camera outright;
	 * whatever the user was flying toward is gone with it. Safe this
	 * early -- fsv_set_mode( ) has not assigned globals.fsv_mode yet at
	 * this point, and camera_flight_end( ) deliberately does not read
	 * it (see the note on its definition). */
	camera_flight_end( );

	camera->fov = 60.0;
	camera->pan_part = 1.0;
	switch (mode) {
		case FSV_DISCV:
		d = field_distance( camera->fov, 2.0 * DISCV_GEOM_PARAMS(root_dnode)->radius );
		if (initial_view) {
			camera->distance = 2.0 * d;
			DISCV_CAMERA(camera)->target.x = 0.0;
			DISCV_CAMERA(camera)->target.y = 0.0;
		}
		else {
			camera->distance = 3.0 * d;
			DISCV_CAMERA(camera)->target.x = 0.0;
			DISCV_CAMERA(camera)->target.y = 0.0;
		}
		camera->near_clip = 0.9375 * camera->distance;
		camera->far_clip = 1.0625 * camera->distance;
		break;

		case FSV_MAPV:
		d1 = field_distance( camera->fov, MAPV_NODE_WIDTH(root_dnode) );
		d2 = MAPV_GEOM_PARAMS(root_dnode)->height + geometry_mapv_max_expanded_height( root_dnode );
		d = MAX(d1, d2);
		if (initial_view) {
			camera->theta = 270.0;
			camera->phi = 0.0;
			camera->distance = 4.0 * d;
			MAPV_CAMERA(camera)->target.x = 0.0;
			MAPV_CAMERA(camera)->target.y = 0.0;
			MAPV_CAMERA(camera)->target.z = 0.0;
		}
		else {
			if (globals.current_node == root_dnode) {
				camera->theta = 270.0;
				camera->phi = 90.0;
				camera->distance = 1.05 * d2 / NEAR_TO_DISTANCE_RATIO;
				MAPV_CAMERA(camera)->target.x = 0.0;
				MAPV_CAMERA(camera)->target.y = MAPV_GEOM_PARAMS(root_dnode)->c1.y + camera->distance;
				MAPV_CAMERA(camera)->target.z = 0.0;
			}
			else {
				camera->theta = 270.0;
				camera->phi = 90.0;
				camera->distance = 1.5 * d;
				MAPV_CAMERA(camera)->target.x = 0.0;
				MAPV_CAMERA(camera)->target.y = 0.0;
				MAPV_CAMERA(camera)->target.z = 0.0;
			}
		}
		camera->near_clip = NEAR_TO_DISTANCE_RATIO * camera->distance;
		camera->far_clip = FAR_TO_NEAR_RATIO * camera->near_clip;
		break;

		case FSV_TREEV:
		geometry_treev_get_extents( root_dnode, NULL, &ext_c1 );
		d = field_distance( camera->fov, 2.0 * ext_c1.r );
		if (initial_view) {
			camera->theta = 0.0;
			camera->phi = 90.0;
			camera->distance = 2.0 * d;
			TREEV_CAMERA(camera)->target.r = 0.5 * TREEV_GEOM_PARAMS(root_dnode)->platform.depth + geometry_treev_platform_r0( root_dnode );
			TREEV_CAMERA(camera)->target.theta = 90.0;
			TREEV_CAMERA(camera)->target.z = 0.0;
		}
		else {
			camera->theta = 0.0;
			camera->phi = 90.0;
			camera->distance = d;
			TREEV_CAMERA(camera)->target.r = 0.5 * d;
			TREEV_CAMERA(camera)->target.theta = 90.0;
			TREEV_CAMERA(camera)->target.z = 0.0;
		}
		camera->near_clip = NEAR_TO_DISTANCE_RATIO * camera->distance;
		camera->far_clip = FAR_TO_NEAR_RATIO * camera->near_clip;
		break;

		case FSV_FSN:
		/* Frame the whole landscape from behind the root pedestal,
		 * looking down the depth axis the tree grows along. See the
		 * FSN_CAMERA_* note at the top of this file -- placeholder,
		 * Task B2 replaces it. */
		fsn_layout_extents( &fsn_ext.x, &fsn_ext.y, &fsn_ext.z );
		fsn_ped = fsn_layout_get( root_dnode );
		/* MAX(1.0, ...): an empty or not-yet-laid-out landscape would
		 * otherwise give distance == near_clip == far_clip == 0, and
		 * a frustum with near == far is a division by zero */
		d = field_distance( camera->fov,
		    MAX(1.0, MAX(fsn_ext.x, fsn_ext.y)) );
		camera->theta = FSN_CAMERA_THETA;
		camera->phi = FSN_CAMERA_PHI;
		/* Far enough back that the root pedestal -- which sits at the
		 * near end of the landscape, not at its center -- still
		 * clears NEAR_TO_DISTANCE_RATIO's near plane */
		camera->distance = (initial_view ? 2.0 : 1.25) * d;
		MAPV_CAMERA(camera)->target.x = (fsn_ped != NULL) ? fsn_ped->x : 0.0;
		MAPV_CAMERA(camera)->target.y = 0.5 * fsn_ext.y;
		MAPV_CAMERA(camera)->target.z = 0.5 * fsn_ext.z;
		camera->near_clip = NEAR_TO_DISTANCE_RATIO * camera->distance;
		camera->far_clip = FAR_TO_NEAR_RATIO * camera->near_clip;
		break;

                SWITCH_FAIL
	}
}


/* Formula for camera yaw in MapV mode */
static double
mapv_camera_theta( double target_x )
{
	return 270.0 + 45.0 * target_x / MAPV_NODE_WIDTH(root_dnode);
}


/* Formula for camera pitch in MapV mode */
static double
mapv_camera_phi( double target_y, GNode *target_node )
{
	if (target_node == root_dnode)
		return 52.5;

	return 45.0 + 15.0 * (target_y - MAPV_GEOM_PARAMS(target_node->parent)->c0.y) / MAPV_NODE_DEPTH(target_node->parent);
}


/* Formula for camera yaw in TreeV mode */
static double
treev_camera_theta( double target_theta, GNode *target_node )
{
	double rel_theta;

	if (geometry_treev_is_leaf( target_node )) {
                rel_theta = target_theta - geometry_treev_platform_theta( target_node->parent );
		return -15.0 * rel_theta / TREEV_GEOM_PARAMS(target_node->parent)->platform.arc_width;
	}
	else
		return -0.125 * (target_theta - 90.0);
}


/* Helper function for camera_scrollbar_moved( ) */
static void
discv_scrollbar_move( double value, int axis )
{
	switch (axis) {
		case X_AXIS:
		/* ????? */
		DISCV_CAMERA(camera)->target.x = value;
		break;

		case Y_AXIS:
		/* ????? */
		DISCV_CAMERA(camera)->target.y = value;
		break;

		SWITCH_FAIL
	}
}


/* Helper function for camera_scrollbar_moved( ) */
static void
mapv_scrollbar_move( double value, int axis )
{
	switch (axis) {
		case X_AXIS:
		MAPV_CAMERA(camera)->target.x = value;
		if (!birdseye_view_active) {
			/* Yaw appropriately */
			camera->theta = mapv_camera_theta( value );
		}
		break;

		case Y_AXIS:
		MAPV_CAMERA(camera)->target.y = - value;
		if (!birdseye_view_active && (globals.current_node != root_dnode)) {
			/* Pitch appropriately */
			camera->phi = mapv_camera_phi( - value, globals.current_node );
		}
		break;

		SWITCH_FAIL
	}
}


/* Helper function for camera_scrollbar_moved( ) */
static void
treev_scrollbar_move( double value, int axis )
{
	switch (axis) {
		case X_AXIS:
		TREEV_CAMERA(camera)->target.theta = - value;
		if (birdseye_view_active) {
			/* Keep root directory at the 12-o'clock position */
			camera->theta = 90.0 - TREEV_CAMERA(camera)->target.theta;
		}
		else {
			/* Yaw appropriately */
			camera->theta = treev_camera_theta( - value, globals.current_node );
		}
		break;

		case Y_AXIS:
		TREEV_CAMERA(camera)->target.r = - value;
		break;

		SWITCH_FAIL
	}
}


/* Helper function for camera_scrollbar_moved( ).
 * FSN's ground plane is Cartesian like MapV's, but deliberately without
 * MapV's coupled yaw/pitch adjustment: mapv_camera_theta( )/_phi( ) are
 * expressed in MAPV_GEOM_PARAMS, which hold FSN geometry in this mode.
 * Panning only, therefore -- and no FSN scrollbar can be dragged today
 * anyway (see fsn_get_scrollbar_state( ) below). Task B2 replaces this. */
static void
fsn_scrollbar_move( double value, int axis )
{
	switch (axis) {
		case X_AXIS:
		MAPV_CAMERA(camera)->target.x = value;
		break;

		case Y_AXIS:
		MAPV_CAMERA(camera)->target.y = - value;
		break;

		SWITCH_FAIL
	}
}


/* Called by the frontend whenever the user manually moves one of the
 * viewport scrollbars (i.e. drags the slider). Reads the new scrollbar
 * position via fsv_platform.get_scroll( ) and updates the camera target
 * for the current visualization mode accordingly.
 * axis is 0 for the x-axis scrollbar, 1 for the y-axis scrollbar (same
 * convention as fsv_platform.set_scroll( )/get_scroll( )) */
void
camera_scrollbar_moved( int axis )
{
	double value;

	/* Get value at center of scrollbar slider */
	value = fsv_platform.get_scroll( axis ) + 0.5 * scroll_state[axis].page;

	switch (globals.fsv_mode) {
		case FSV_DISCV:
		discv_scrollbar_move( value, axis );
		break;

		case FSV_MAPV:
		mapv_scrollbar_move( value, axis );
		break;

		case FSV_TREEV:
		treev_scrollbar_move( value, axis );
		break;

		case FSV_FSN:
		fsn_scrollbar_move( value, axis );
		break;

		SWITCH_FAIL
	}

	/* Camera is under user control */
	camera->manual_control = TRUE;

	redraw( );
}


/* Default scrollbar states */
static void
null_get_scrollbar_state( ScrollState *x, ScrollState *y )
{
	x->lower = 0.0;
	x->upper = 100.0;
	x->value = 0.0;
	x->page = 100.0;
	*y = *x;
}


/* This produces the exact state the viewport scrollbars should have in
 * DiscV mode, given the current camera state and current node */
static void
discv_get_scrollbar_state( ScrollState *x, ScrollState *y )
{

	/* TODO: To be implemented... */

	*x = scroll_state[X_AXIS];
	*y = scroll_state[Y_AXIS];
}


/* Same as above, but for MapV mode */
static void
mapv_get_scrollbar_state( ScrollState *x, ScrollState *y )
{
	GNode *dnode;
	XYvec dims, margin;
	XYvec c0, c1;
	double diameter;
	double cofs;

	if (birdseye_view_active) {
		/* The bird sees everything */
		dnode = root_dnode;
	}
	else {
		/* Scrollable area is that of top face of parent
		 * directory of the current node */
		if (NODE_IS_DIR(globals.current_node->parent))
			dnode = globals.current_node->parent;
		else
			dnode = globals.current_node;
	}

	/* Dimensions of scrollable area */
	dims.x = MAPV_NODE_WIDTH(dnode);
	dims.y = MAPV_NODE_DEPTH(dnode);

	/* Diameter of camera's field of view (centered at target) */
	diameter = field_diameter( camera->fov, camera->distance );

	/* Margin widths */
	margin.x = 0.5 * MIN(diameter, dims.x);
	margin.y = 0.5 * MIN(diameter, dims.y);

	/* Corners of scrollable area */
	c0.x = MIN(MAPV_GEOM_PARAMS(dnode)->c0.x + margin.x, MAPV_CAMERA(camera)->target.x);
	c0.y = MIN(MAPV_GEOM_PARAMS(dnode)->c0.y + margin.y, MAPV_CAMERA(camera)->target.y);
	c1.x = MAX(MAPV_GEOM_PARAMS(dnode)->c1.x - margin.x, MAPV_CAMERA(camera)->target.x);
	c1.y = MAX(MAPV_GEOM_PARAMS(dnode)->c1.y - margin.y, MAPV_CAMERA(camera)->target.y);

	/* Corrective offset (since value actually indicates position
	 * at top of scrollbar slider, not center) */
	cofs = 0.5 * diameter;

	/* x-scrollbar state */
	x->lower = c0.x - cofs;
	x->upper = c1.x + cofs;
	x->value = MAPV_CAMERA(camera)->target.x - cofs;
	x->page = diameter;

	/* y-scrollbar state
	 * Note: lower, upper, and value have signs reversed to correct for
	 * canonical scrollbar increment direction (wrong for our needs) */
	y->lower = - c1.y - cofs;
	y->upper = - c0.y + cofs;
	y->value = - MAPV_CAMERA(camera)->target.y - cofs;
	y->page = diameter;
}


/* Same as above, but for TreeV mode */
static void
treev_get_scrollbar_state( ScrollState *x, ScrollState *y )
{
	GNode *dnode;
	RTvec area_dims, dir_pos;
	RTvec c0, c1;
	RTvec vis_range;
	RTvec margin;
	double diameter;
	double cofs;

	if (!dirtree_entry_expanded( root_dnode )) {
		/* Disable scrolling in this circumstance */
		null_get_scrollbar_state( x, y );
                return;
	}

	/* Get dimensions of scrollable area */
	if (birdseye_view_active) {
		/* Birdie can fly around the entire tree */
		if (geometry_treev_is_leaf( globals.current_node ))
			area_dims.r = geometry_treev_platform_r0( globals.current_node->parent );
		else
			area_dims.r = geometry_treev_platform_r0( globals.current_node );
		dnode = root_dnode;
		area_dims.theta = MAX(TREEV_GEOM_PARAMS(dnode)->platform.arc_width, TREEV_GEOM_PARAMS(dnode)->platform.subtree_arc_width);
	}
	else {
		if (geometry_treev_is_leaf( globals.current_node )) {
			dnode = globals.current_node->parent;
			area_dims.theta = TREEV_GEOM_PARAMS(dnode)->platform.arc_width;
		}
		else {
			dnode = globals.current_node;
			area_dims.theta = MAX(TREEV_GEOM_PARAMS(dnode)->platform.arc_width, TREEV_GEOM_PARAMS(dnode)->platform.subtree_arc_width);
		}
		area_dims.r = TREEV_GEOM_PARAMS(dnode)->platform.depth;
	}

	/* Visible range of camera's field of view */
	diameter = field_diameter( camera->fov, camera->distance );
	vis_range.r = diameter;
	vis_range.theta = (180.0 / PI) * diameter / TREEV_CAMERA(camera)->target.r;

	/* Margin width (r-axis only; angle margin doesn't work well
	 * with camera yaw) */
	margin.r = 0.5 * MIN(vis_range.r, area_dims.r);

	/* Base directory position */
	dir_pos.r = geometry_treev_platform_r0( dnode );
	dir_pos.theta = geometry_treev_platform_theta( dnode );

	/* Corners of scrollable area */
	c0.r = MIN(dir_pos.r + margin.r, TREEV_CAMERA(camera)->target.r);
	c0.theta = MIN(dir_pos.theta - 0.5 * area_dims.theta, TREEV_CAMERA(camera)->target.theta);
	c1.r = MAX(dir_pos.r + area_dims.r - margin.r, TREEV_CAMERA(camera)->target.r);
	c1.theta = MAX(dir_pos.theta + 0.5 * area_dims.theta, TREEV_CAMERA(camera)->target.theta);

	/* x-scrollbar state (signs reversed) */
	cofs = 0.5 * vis_range.theta;
	x->lower = - c1.theta - cofs;
	x->upper = - c0.theta + cofs;
	x->value = - TREEV_CAMERA(camera)->target.theta - cofs;
	x->page = vis_range.theta;

	/* y-scrollbar state (signs reversed) */
	cofs = 0.5 * vis_range.r;
	y->lower = - c1.r - cofs;
	y->upper = - c0.r + cofs;
	y->value = - TREEV_CAMERA(camera)->target.r - cofs;
	y->page = vis_range.r;
}


/* Returns a ScrollState with each field linearly interpolated between the
 * corresponding fields of a and b, according to interpolation factor k
 * (i.e. if k == 0, result == *a; if k == 1, result == *b; etc.
 * k should be between 0 and 1 inclusive, of course) */
static ScrollState
scroll_interpolate( double k, const ScrollState *a, const ScrollState *b )
{
	ScrollState out;

	out.lower = a->lower + k * (b->lower - a->lower);
	out.upper = a->upper + k * (b->upper - a->upper);
	out.page  = a->page  + k * (b->page  - a->page);
	out.value = a->value + k * (b->value - a->value);

	return out;
}


/* Updates the scrollbars to reflect current camera state and current node
 * status (interpolating smoothly if the camera is panning).
 * hard_update indicates if scrollbars must be updated now (TRUE) or may be
 * updated later (FALSE) */
void
camera_update_scrollbars( boolean hard_update )
{
	ScrollState x = {0}, y = {0};

	/* hard_update no longer distinguishes any behavior here: pushing
	 * scroll state through fsv_platform.set_scroll( ) is cheap and the
	 * frontend is free to throttle/coalesce widget updates on its own
	 * if it ever needs to. Kept in the signature for API stability
	 * (many call sites pass either TRUE or FALSE). */
	(void)hard_update;

	/* Get current scrollbar states */
	switch (globals.fsv_mode) {
		case FSV_SPLASH:
		null_get_scrollbar_state(&x, &y);
		break;

		case FSV_DISCV:
		discv_get_scrollbar_state(&x, &y);
		break;

		case FSV_MAPV:
		mapv_get_scrollbar_state(&x, &y);
		break;

		case FSV_FSN:
		/* No scroll model yet -- Task B2 is what gives FSN its own
		 * navigation. Deliberately the *null* state rather than
		 * MapV's: mapv_get_scrollbar_state( ) is written entirely in
		 * MAPV_GEOM_PARAMS, which carry FSN pedestals in this mode,
		 * so it would report ranges computed from a reinterpreted
		 * FsnPedestal. src/sdl/ui_rail.cpp keeps its Tilt/Height
		 * sliders disabled in FSN mode to match. */
		null_get_scrollbar_state(&x, &y);
		break;

		case FSV_TREEV:
		treev_get_scrollbar_state(&x, &y);
		break;

		SWITCH_FAIL
	}

	if (camera_moving( )) {
		/* Interpolate between current and previous scrollbar
		 * states according to position in camera pan */
		x = scroll_interpolate( camera->pan_part, &prev_scroll_state[X_AXIS], &x );
		y = scroll_interpolate( camera->pan_part, &prev_scroll_state[Y_AXIS], &y );
	}

	scroll_state[X_AXIS] = x;
	scroll_state[Y_AXIS] = y;

	/* Push to the frontend's scrollbar widgets */
	fsv_platform.set_scroll( X_AXIS, x.lower, x.upper, x.page, x.value );
	fsv_platform.set_scroll( Y_AXIS, y.lower, y.upper, y.page, y.value );
}


/* This causes an ongoing camera pan to finish immediately
 * (i.e. camera jumps instantly to its destination) */
void
camera_pan_finish( void )
{
	morph_finish( &camera->theta );
	morph_finish( &camera->phi );
	morph_finish( &camera->distance );
	morph_finish( &camera->fov );
	morph_finish( &camera->near_clip );
	morph_finish( &camera->far_clip );
	morph_finish( &camera->pan_part );

	switch (globals.fsv_mode) {
		case FSV_DISCV:
		morph_finish( &DISCV_CAMERA(camera)->target.x );
		morph_finish( &DISCV_CAMERA(camera)->target.y );
		break;

		case FSV_FSN:
		/* FSN stores its target in MapV's Cartesian camera struct
		 * (see the FSN_CAMERA_* note at the top of this file), so the
		 * same three variables are the ones to settle */
		case FSV_MAPV:
		morph_finish( &MAPV_CAMERA(camera)->target.x );
		morph_finish( &MAPV_CAMERA(camera)->target.y );
		morph_finish( &MAPV_CAMERA(camera)->target.z );
		break;

		case FSV_TREEV:
		morph_finish( &TREEV_CAMERA(camera)->target.r );
		morph_finish( &TREEV_CAMERA(camera)->target.theta );
		morph_finish( &TREEV_CAMERA(camera)->target.z );
		break;

		SWITCH_FAIL
	}
}


/* This stops an ongoing camera pan immediately
 * (camera does not reach its destination) */
void
camera_pan_break( void )
{
	morph_break( &camera->theta );
	morph_break( &camera->phi );
	morph_break( &camera->distance );
	morph_break( &camera->fov );
	morph_break( &camera->near_clip );
	morph_break( &camera->far_clip );
	morph_break( &camera->pan_part );

	switch (globals.fsv_mode) {
		case FSV_DISCV:
		morph_break( &DISCV_CAMERA(camera)->target.x );
		morph_break( &DISCV_CAMERA(camera)->target.y );
		break;

		case FSV_FSN:
		/* Shares MapV's Cartesian target storage -- as above */
		case FSV_MAPV:
		morph_break( &MAPV_CAMERA(camera)->target.x );
		morph_break( &MAPV_CAMERA(camera)->target.y );
		morph_break( &MAPV_CAMERA(camera)->target.z );
		break;

		case FSV_TREEV:
		morph_break( &TREEV_CAMERA(camera)->target.r );
		morph_break( &TREEV_CAMERA(camera)->target.theta );
		morph_break( &TREEV_CAMERA(camera)->target.z );
		break;

		SWITCH_FAIL
	}
}


/**** fsn flight navigation ****
 *
 * The one piece of camera motion in fsv that is NOT a morph. A morph has
 * a start value, an end value and a duration; flight has a velocity and
 * runs until the user lets go of the button. Expressing it as a morph
 * would mean either re-arming a fresh one-frame morph on every tick (a
 * malloc, a queue insert, a queue removal and an end callback per frame,
 * to interpolate between two values a frame apart) or morphing toward a
 * fictitious far-away target and breaking it on release -- which would
 * make the pointer's offset control *acceleration* rather than speed,
 * since the morph's own easing would still be shaping the motion.
 *
 * So the rates live here and the frontend's main loop calls
 * camera_flight_tick( ) once per iteration, next to fsv_animation_tick( ).
 * The animation subsystem still drives the actual drawing: each tick
 * that moves the camera calls redraw( ), which is what keeps frames
 * flowing (animation.c's animation_active) for exactly as long as the
 * camera is moving and not one frame longer.
 *
 * Position and heading only. The viewer moves along the ground plane in
 * the direction it is facing, turns on the spot, and (with Shift) climbs
 * or dives; camera->phi is deliberately left alone, because fsn's flight
 * was planar -- the pitch is part of the viewpoint, not part of the
 * flying. FSN's target is stored in MapV's Cartesian camera struct (see
 * the FSN_CAMERA_* note at the top of this file), so "position" here is
 * MAPV_CAMERA(camera)->target and "heading" is camera->theta. */

/* TRUE while the middle button is held in FSV_FSN mode */
static boolean flight_active = FALSE;

/* Current flight rates, all per second. Set by camera_flight_update( )
 * from the pointer offset, integrated by camera_flight_tick( ). */
static double flight_speed = 0.0;	/* along the horizontal view direction */
static double flight_yaw_rate = 0.0;	/* degrees, added to camera->theta */
static double flight_climb = 0.0;	/* world z */

/* xgettime( ) at the last tick, for the time step */
static double flight_t_prev = 0.0;


/* Cancels an in-progress camera pan *and* performs the bookkeeping its
 * end callback would otherwise have done.
 *
 * camera_pan_break( ) alone is not enough for a caller that simply
 * stops: morph_break( ) drops a morph record without calling its
 * end_cb, so the master pan morph's pan_end_cb( )/post_pan_end( ) never
 * runs -- and that pair is what clears camera_currently_moving and hands
 * the user interface back (window_set_access( TRUE )). Every other
 * caller of camera_pan_break( ) in this file immediately arms a
 * replacement pan, master morph included, so none of them noticed.
 * Flight is the first one that doesn't.
 *
 * Two of post_pan_end( )'s four actions are deliberately NOT reproduced:
 *
 *   - geometry_camera_pan_finished( ): it records where the node cursor
 *     came to rest, and an interrupted pan came to rest nowhere. (FSN's
 *     arm of it is empty in any case -- the mode draws no cursor.)
 *   - filelist_show_entry( node ): it scrolls the file list to the node
 *     the pan was *heading for*, which is precisely the node the user
 *     just decided not to go to. A no-op in the SDL frontend today
 *     (src/sdl/stubs.c) but real in the GTK one and a candidate to
 *     become real here, so this is a decision, not an oversight: the
 *     pan's destination never became the current node, so nothing should
 *     select it.
 *
 * The other two -- window_set_access( TRUE ) and the
 * camera_currently_moving clear -- are what this function exists for. */
static void
cancel_pan_for_manual_control( void )
{
	if (!camera_currently_moving)
		return;

	camera_pan_break( );
	camera_currently_moving = FALSE;
	camera->pan_part = 1.0;
	window_set_access( TRUE );
}


/* Shifts camera->theta by whole turns until it is within 180 degrees of
 * target_theta, so that a morph between the two takes the short way
 * round. Call immediately before arming such a morph.
 *
 * Why it is needed: morph( ) interpolates linearly between two
 * *numbers*, and theta is an angle -- two headings a few degrees apart
 * on the compass can be most of a turn apart numerically. Flight makes
 * that routine, because camera_flight_tick( ) normalizes theta into
 * [0, 360] on every tick: a viewer who has turned slightly past the
 * wrap point leaves theta at, say, 3 degrees, and a pan to
 * FSN_CAMERA_THETA (270) would then spin the long way round (267
 * degrees, over a second of gratuitous yaw) instead of taking the
 * 93-degree short arc.
 *
 * Why it is free: every other consumer of theta takes its sine or
 * cosine, so theta and theta +/- 360 are the same heading everywhere
 * (mapv_get_camera_position( ) included). Only the morph, which does
 * arithmetic on the number itself, can tell them apart -- which is
 * exactly the bug.
 *
 * Applied to the two FSN pans that can follow a flight (fsn_look_at( )
 * and camera_birdseye_view( )'s going-up arm) and deliberately nowhere
 * else. camera_revolve( ) normalizes theta the same way, so DiscV, MapV
 * and TreeV have the same long-way-round pan after a manual revolve --
 * pre-existing upstream behavior in three modes this task is not
 * touching, recorded in docs/PORTING.md rather than changed under cover
 * of an fsn task. */
static void
unwrap_theta_toward( double target_theta )
{
	while ((target_theta - camera->theta) > 180.0)
		camera->theta += 360.0;
	while ((camera->theta - target_theta) > 180.0)
		camera->theta -= 360.0;
}


/* Maps one axis of the pointer's offset from the press point onto a
 * rate: zero inside the dead zone, then linear in the offset past it,
 * clamped at max_rate. Sign is carried through. */
static double
flight_axis_rate( double offset_px, double scale, double max_rate )
{
	double magnitude;

	magnitude = ABS(offset_px);
	if (magnitude <= FSN_FLIGHT_DEAD_ZONE_PX)
		return 0.0;

	magnitude = MIN((magnitude - FSN_FLIGHT_DEAD_ZONE_PX) * scale, max_rate);

	return (offset_px < 0.0) ? -magnitude : magnitude;
}


/* Starts a flight. No-op outside FSV_FSN, so the frontend's per-mode
 * gesture dispatch is backed up by the invariant living here too. */
void
camera_flight_begin( void )
{
	if (globals.fsv_mode != FSV_FSN)
		return;

	/* Flight and a camera pan are two things moving the same variables;
	 * the user wins. (The converse -- a look_at during a flight -- is
	 * handled by camera_look_at_full( ) calling camera_flight_end( ).) */
	cancel_pan_for_manual_control( );

	flight_active = TRUE;
	flight_speed = 0.0;
	flight_yaw_rate = 0.0;
	flight_climb = 0.0;
	flight_t_prev = xgettime( );

	/* Exactly what camera_dolly( )/camera_revolve( ) do, and for the
	 * same reason: this is the user steering, so colexp.c must not
	 * re-aim the camera underneath them (colexp.c's !manual_control
	 * branch). Note what is NOT done here -- window_set_access( FALSE ).
	 * That call means "an animation owns the camera, keep the user off
	 * the controls"; flight is the opposite of that. */
	camera->manual_control = TRUE;
}


/* Feeds the pointer's current offset from the press point (in the
 * frontend's pixel space -- see fsn-style.h) into the flight rates.
 * y grows downward in that space, so a negative dy (pointer above the
 * press point) is forward, and up. */
void
camera_flight_update( double dx_from_press, double dy_from_press, boolean vertical )
{
	if (!flight_active)
		return;

	/* Yaw: pointer to the right of the press point turns right. theta
	 * is a counterclockwise heading, so turning right decreases it --
	 * the same sign camera_revolve( ) gives a rightward drag. */
	flight_yaw_rate = -flight_axis_rate( dx_from_press,
	    FSN_FLIGHT_YAW_SCALE, FSN_FLIGHT_YAW_MAX );

	if (vertical) {
		/* Shift: the y offset is altitude instead of speed. Not "as
		 * well as": holding Shift stops the viewer moving forward, so
		 * the gesture is a pure ascent/descent. */
		flight_speed = 0.0;
		flight_climb = flight_axis_rate( -dy_from_press,
		    FSN_FLIGHT_ALT_SCALE, FSN_FLIGHT_ALT_MAX );
	}
	else {
		flight_speed = flight_axis_rate( -dy_from_press,
		    FSN_FLIGHT_SPEED_SCALE, FSN_FLIGHT_SPEED_MAX );
		flight_climb = 0.0;
	}
}


/* Stops a flight. Idempotent, and safe in any mode: this is what
 * input_reset( ), the Escape key and every camera_look_at( ) call
 * reach for, none of which can know whether a flight is in progress.
 *
 * Deliberately does NOT push scrollbar state. The last tick that moved
 * the camera already did (camera_flight_tick( )'s
 * camera_update_scrollbars( FALSE )), and a flight that never moved the
 * camera has nothing to push -- so the call would be redundant in both
 * cases. Leaving it out also keeps this function free of any dependency
 * on globals.fsv_mode, which matters because two of its callers run at
 * moments when that variable does not describe the world:
 * camera_init( ), which fsv_set_mode( ) calls after the NEW mode's
 * geometry_init( ) but BEFORE assigning globals.fsv_mode; and
 * src/sdl/input.cpp's input_reset( ), which runs around a rescan.
 * camera_update_scrollbars( ) ends in SWITCH_FAIL for FSV_NONE. */
void
camera_flight_end( void )
{
	if (!flight_active)
		return;

	flight_active = FALSE;
	flight_speed = 0.0;
	flight_yaw_rate = 0.0;
	flight_climb = 0.0;
}


boolean
camera_flight_active( void )
{
	return flight_active;
}


/* Integrates the current flight rates over the time elapsed since the
 * last call. Called once per frontend main-loop iteration. */
void
camera_flight_tick( void )
{
	double t_now, dt;
	double sin_theta, cos_theta;

	if (!flight_active)
		return;

	t_now = xgettime( );
	dt = t_now - flight_t_prev;
	flight_t_prev = t_now;
	dt = CLAMP(dt, 0.0, FSN_FLIGHT_MAX_STEP);

	if ((flight_speed == 0.0) && (flight_yaw_rate == 0.0) && (flight_climb == 0.0))
		/* Button held, pointer inside the dead zone: nothing moves, so
		 * deliberately no redraw( ) either. Holding still costs the
		 * same as not flying at all. */
		return;

	/* Heading first, so this step's travel uses the heading the viewer
	 * ends the step facing -- a turn and a translation in the same
	 * frame then read as one curved move rather than a sideways skid */
	camera->theta += flight_yaw_rate * dt;
	while (camera->theta < 0.0)
		camera->theta += 360.0;
	while (camera->theta > 360.0)
		camera->theta -= 360.0;

	/* The camera sits at target + distance * (cos(theta)cos(phi),
	 * sin(theta)cos(phi), sin(phi)) and looks back down that vector at
	 * the target (mapv_get_camera_position( ) above), so the direction
	 * the viewer faces, projected onto the ground, is
	 * -(cos(theta), sin(theta)). Moving the target along it carries the
	 * whole rig -- viewpoint and all -- forward.
	 *
	 * Sanity check on the signs: at the initial FSN_CAMERA_THETA of
	 * 270 degrees that comes out as -(0, -1) == +y, which is the
	 * direction the landscape grows away from the camera (the "z" axis
	 * of geometry-fsn.h's FsnPedestal). Pushing forward flies into the
	 * tree, which is the point of the mode. */
	cos_theta = cos( RAD(camera->theta) );
	sin_theta = sin( RAD(camera->theta) );
	MAPV_CAMERA(camera)->target.x -= flight_speed * dt * cos_theta;
	MAPV_CAMERA(camera)->target.y -= flight_speed * dt * sin_theta;

	/* No ceiling on the climb: flying up is self-limiting (the whole
	 * landscape comes into frame and there is nothing further to see),
	 * and a cap would have to be recomputed on every rescan. The floor
	 * is real, though -- below the ground plane the viewer is looking
	 * up at the underside of a landscape drawn as if lit from above. */
	MAPV_CAMERA(camera)->target.z =
	    MAX(0.0, MAPV_CAMERA(camera)->target.z + flight_climb * dt);

	/* Same pair the scrollbar and dolly paths use: push the new camera
	 * state out to the frontend's scroll widgets, then ask for a frame.
	 * FALSE (soft) rather than TRUE because this fires every frame --
	 * and because camera_moving( ) is false during a flight, so
	 * camera_update_scrollbars( ) takes its non-interpolating path
	 * either way. */
	camera_update_scrollbars( FALSE );
	redraw( );
}


/* Helper function for camera_look_at_full( ) */
static double
discv_look_at( GNode *node, MorphType mtype, double pan_time_override )
{
	DiscVCamera new_dcam;
	Camera *new_cam;
	XYvec *node_pos;
	double pan_time;

	new_cam = CAMERA(&new_dcam);

	/* Construct desired camera state */

	/* Distance from target point */
	new_cam->distance = 2.0 * field_distance( camera->fov, 2.0 * DISCV_GEOM_PARAMS(node)->radius );

	/* Clipping plane distances */
	new_cam->near_clip = 0.9375 * new_cam->distance;
	new_cam->far_clip = 1.0625 * new_cam->distance;

	/* Target point */
	node_pos = geometry_discv_node_pos( node );
	DISCV_CAMERA(new_cam)->target.x = node_pos->x;
	DISCV_CAMERA(new_cam)->target.y = node_pos->y;

	/* Duration of pan */
	if (pan_time_override > 0.0)
		pan_time = pan_time_override;
	else {
/* TODO: write a *real* pan_time function here */
		pan_time = 2.0;
		/*pan_time = CLAMP(k, DISCV_CAMERA_MIN_PAN_TIME, DISCV_CAMERA_MAX_PAN_TIME);*/
	}

	/* Get the camera moving */
	morph( &camera->distance, mtype, new_cam->distance, pan_time );
	morph( &camera->near_clip, mtype, new_cam->near_clip, pan_time );
	morph( &camera->far_clip, mtype, new_cam->far_clip, pan_time );
	morph( &DISCV_CAMERA(camera)->target.x, mtype, DISCV_CAMERA(new_cam)->target.x, pan_time );
	morph( &DISCV_CAMERA(camera)->target.y, mtype, DISCV_CAMERA(new_cam)->target.y, pan_time );

	return pan_time;
}


/* Helper function for mapv_look_at( ). Calculates the position of a camera
 * (i.e. viewer location) */
static void
mapv_get_camera_position( const Camera *cam, XYZvec *pos )
{
	double sin_theta, cos_theta, sin_phi, cos_phi;

	sin_theta = sin( RAD(cam->theta) );
	cos_theta = cos( RAD(cam->theta) );
	sin_phi = sin( RAD(cam->phi) );
	cos_phi = cos( RAD(cam->phi) );

	pos->x = MAPV_CAMERA(cam)->target.x + cam->distance * cos_theta * cos_phi;
	pos->y = MAPV_CAMERA(cam)->target.y + cam->distance * sin_theta * cos_phi;
	pos->z = MAPV_CAMERA(cam)->target.z + cam->distance * sin_phi;
}


/* Helper function for camera_look_at_full( ) */
static double
mapv_look_at( GNode *node, MorphType mtype, double pan_time_override )
{
	MapVCamera new_mcam;
	Camera *new_cam;
	Camera apg_cam;
	XYZvec node_pos;
	XYZvec camera_pos, new_cam_pos, delta;
	XYvec node_dims;
	double diameter, height;
	double pan_time;
	double xy_travel;
	double k;
	boolean swing_back = FALSE;

	new_cam = CAMERA(&new_mcam);

	/* Get target node geometry */
	node_pos.x = 0.5 * (MAPV_GEOM_PARAMS(node)->c0.x + MAPV_GEOM_PARAMS(node)->c1.x);
	node_pos.y = 0.5 * (MAPV_GEOM_PARAMS(node)->c0.y + MAPV_GEOM_PARAMS(node)->c1.y);
	node_pos.z = geometry_mapv_node_z0( node ) + MAPV_GEOM_PARAMS(node)->height;
	node_dims.x = MAPV_NODE_WIDTH(node);
	node_dims.y = MAPV_NODE_DEPTH(node);

	/* Construct desired camera state */

	/* Target point (may get bumped upward; see further down) */
	MAPV_CAMERA(new_cam)->target.x = node_pos.x;
	MAPV_CAMERA(new_cam)->target.y = node_pos.y;
	MAPV_CAMERA(new_cam)->target.z = node_pos.z;

	/* Viewing angles */
	new_cam->theta = mapv_camera_theta( node_pos.x );
        new_cam->phi = mapv_camera_phi( node_pos.y, node );

	/* Distance from target point */
	k = sqrt( node_dims.x * node_dims.y );
	diameter = SQRT_2 * MAX(k, 0.5 * MAX(node_dims.x, node_dims.y));
	if (NODE_IS_DIR(node)) {
		height = geometry_mapv_max_expanded_height( node );
		diameter = MAX(diameter, height);
		if (dirtree_entry_expanded( node ))
			diameter = MAX(diameter, MAX(node_dims.x, 1.5 * node_dims.y));
		MAPV_CAMERA(new_cam)->target.z += 0.5 * height;
		k = 1.25;
	}
	else
		k = 2.0;
	new_cam->distance = k * field_distance( camera->fov, diameter );

	/* Clipping plane distances */
	new_cam->near_clip = NEAR_TO_DISTANCE_RATIO * new_cam->distance;
	new_cam->far_clip = FAR_TO_NEAR_RATIO * new_cam->near_clip;

	/* Overall travel vector */
	mapv_get_camera_position( camera, &camera_pos );
	mapv_get_camera_position( new_cam, &new_cam_pos );
	delta.x = new_cam_pos.x - camera_pos.x;
	delta.y = new_cam_pos.y - camera_pos.y;
	delta.z = new_cam_pos.z - camera_pos.z;

	/* Determine how long the camera should take to perform the pan,
	 * if no overriding value was given */
	if (pan_time_override > 0.0)
		pan_time = pan_time_override;
	else {
		k = sqrt( XYZ_LEN(delta) / hypot( MAPV_NODE_WIDTH(root_dnode), MAPV_NODE_DEPTH(root_dnode) ) );
		pan_time = MAX(MAPV_CAMERA_MIN_PAN_TIME, MIN(1.0, k) * MAPV_CAMERA_MAX_PAN_TIME);
	}

	/* Judge if camera should swing back during the pan, and if so,
	 * determine apogee parameters */
	xy_travel = XY_LEN(delta);
	if (xy_travel > (3.0 * MAX(camera->distance, new_cam->distance))) {
		swing_back = TRUE;
		apg_cam.distance = 1.2 * MAX(new_cam->distance, xy_travel);
		apg_cam.near_clip = NEAR_TO_DISTANCE_RATIO * apg_cam.distance;
		apg_cam.far_clip = FAR_TO_NEAR_RATIO * apg_cam.near_clip;
	}

	/* Get the camera moving */
	morph( &camera->theta, mtype, new_cam->theta, pan_time );
	morph( &camera->phi, mtype, new_cam->phi, pan_time );
	if (swing_back) {
		morph( &camera->distance, mtype, apg_cam.distance, 0.5 * pan_time );
		morph( &camera->distance, mtype, new_cam->distance, 0.5 * pan_time );
		morph( &camera->near_clip, mtype, apg_cam.near_clip, 0.5 * pan_time );
		morph( &camera->near_clip, mtype, new_cam->near_clip, 0.5 * pan_time );
		morph( &camera->far_clip, mtype, apg_cam.far_clip, 0.5 * pan_time );
		morph( &camera->far_clip, mtype, new_cam->far_clip, 0.5 * pan_time );
	}
	else {
		morph( &camera->distance, mtype, new_cam->distance, pan_time );
		morph( &camera->near_clip, mtype, new_cam->near_clip, pan_time );
		morph( &camera->far_clip, mtype, new_cam->far_clip, pan_time );
	}
	morph( &MAPV_CAMERA(camera)->target.x, mtype, MAPV_CAMERA(new_cam)->target.x, pan_time );
	morph( &MAPV_CAMERA(camera)->target.y, mtype, MAPV_CAMERA(new_cam)->target.y, pan_time );
	morph( &MAPV_CAMERA(camera)->target.z, mtype, MAPV_CAMERA(new_cam)->target.z, pan_time );

	return pan_time;
}


/* Helper function for camera_look_at_full( ), FSV_FSN mode.
 *
 * Task B1 PLACEHOLDER -- the real fsn flight (a low, ground-hugging
 * travel with approach-deceleration, and the wire-following path between
 * pedestals) is Task B2's whole subject. What this has to do until then
 * is put the target node in frame from a sane angle without ever
 * producing a degenerate frustum. Shaped like mapv_look_at( ) above,
 * with the same morph set, but reading the FSN layout instead of
 * MAPV_GEOM_PARAMS (see the FSN_CAMERA_* note at the top of this file). */
static double
fsn_look_at( GNode *node, MorphType mtype, double pan_time_override )
{
	MapVCamera new_mcam;
	Camera *new_cam;
	const FsnPedestal *ped;
	const FsnPedestal *pped = NULL;
	const FsnPedestal *frame_ped;
	GNode *fnode;
	XYZvec camera_pos, new_cam_pos, delta;
	XYZvec ext;
	double diameter, pan_time, k;

	new_cam = CAMERA(&new_mcam);

	/* A file's box stands on its parent's pedestal, so its own `h` is
	 * measured from there, not from the ground */
	ped = fsn_layout_get( node );
	if (ped == NULL) {
		/* No FSN geometry for this node (should not happen: the mode
		 * lays out the whole tree). Stay where we are. */
		return FSN_CAMERA_MIN_PAN_TIME;
	}

	/* Looked up once and reused below for both the target's pedestal-top
	 * lift and the framing diameter -- a file is the only case where
	 * either needs the parent at all. */
	if (!NODE_IS_DIR(node) && node->parent != NULL &&
	    NODE_IS_DIR(node->parent))
		pped = fsn_layout_get( node->parent );

	MAPV_CAMERA(new_cam)->target.x = ped->x;
	MAPV_CAMERA(new_cam)->target.y = ped->z;
	MAPV_CAMERA(new_cam)->target.z = ped->h;
	if (pped != NULL)
		MAPV_CAMERA(new_cam)->target.z += pped->h;

	new_cam->theta = FSN_CAMERA_THETA;
	new_cam->phi = FSN_CAMERA_PHI;

	/* Enough of the object in frame to make it identifiable -- and, for
	 * an expanded directory, enough to take in the wires leaving it and
	 * the near edge of the generation they lead to, which is the whole
	 * point of the mode. That's still true for a directory; for a FILE
	 * the box is deliberately small in frame at the parent's distance --
	 * identification there is carried by the selection spotlight, and
	 * the close-up by warp-lite, not by this establishing shot.
	 *
	 * A FILE is framed by its PARENT's footprint, not its own: a file
	 * box's own record is box-scale, and sizing the shot to it put the
	 * camera nose-first against the packed box row (TODO.md bug #2;
	 * PORTING.md's Task B3 verification recorded parent-framing, with
	 * the selection left on the file, as the legible shot). The target
	 * above stays on the file's box -- only the framing scale changes.
	 *
	 * In practice the expanded-wire arm below always applies for a file:
	 * fsn_ensure_parent_expanded( ) pre-expands the parent before
	 * fsn_look_at( ) ever runs, so the plain SQRT_2 footprint term is
	 * really the extreme-directory fallback -- it only wins over the
	 * wire-arm term when the pedestal is packed deep enough (on the
	 * order of ~3000 files) that its footprint outgrows the generation
	 * gap. */
	fnode = node;
	frame_ped = ped;
	if (pped != NULL) {
		fnode = node->parent;
		frame_ped = pped;
	}
	diameter = SQRT_2 * MAX(frame_ped->w, frame_ped->d);
	if (NODE_IS_DIR(fnode) && dirtree_entry_expanded( fnode ))
		diameter = MAX(diameter, frame_ped->d + 2.0 * FSN_GENERATION_GAP);
	new_cam->distance = field_distance( camera->fov, MAX(1.0, diameter) );
	new_cam->near_clip = NEAR_TO_DISTANCE_RATIO * new_cam->distance;
	new_cam->far_clip = FAR_TO_NEAR_RATIO * new_cam->near_clip;

	/* Duration: proportional to how far the viewer actually travels,
	 * measured against the size of the whole landscape -- MapV's rule,
	 * with its root-node footprint swapped for the FSN extents */
	if (pan_time_override > 0.0)
		pan_time = pan_time_override;
	else {
		mapv_get_camera_position( camera, &camera_pos );
		mapv_get_camera_position( new_cam, &new_cam_pos );
		delta.x = new_cam_pos.x - camera_pos.x;
		delta.y = new_cam_pos.y - camera_pos.y;
		delta.z = new_cam_pos.z - camera_pos.z;

		fsn_layout_extents( &ext.x, &ext.y, NULL );
		k = sqrt( XYZ_LEN(delta) / MAX(1.0, hypot( ext.x, ext.y )) );
		pan_time = MAX(FSN_CAMERA_MIN_PAN_TIME,
		    MIN(1.0, k) * FSN_CAMERA_MAX_PAN_TIME);
	}

	unwrap_theta_toward( new_cam->theta );

	morph( &camera->theta, mtype, new_cam->theta, pan_time );
	morph( &camera->phi, mtype, new_cam->phi, pan_time );
	morph( &camera->distance, mtype, new_cam->distance, pan_time );
	morph( &camera->near_clip, mtype, new_cam->near_clip, pan_time );
	morph( &camera->far_clip, mtype, new_cam->far_clip, pan_time );
	morph( &MAPV_CAMERA(camera)->target.x, mtype, MAPV_CAMERA(new_cam)->target.x, pan_time );
	morph( &MAPV_CAMERA(camera)->target.y, mtype, MAPV_CAMERA(new_cam)->target.y, pan_time );
	morph( &MAPV_CAMERA(camera)->target.z, mtype, MAPV_CAMERA(new_cam)->target.z, pan_time );

	return pan_time;
}


/* Helper function for treev_look_at( ). Calculates position of a camera. */
static void
treev_get_camera_position( const Camera *cam, RTZvec *pos )
{
	XYZvec target, xyz_pos;
	double theta;
	double sin_theta, cos_theta, sin_phi, cos_phi;

	/* Convert target from RTZ to XYZ */
	theta = TREEV_CAMERA(cam)->target.theta;
	target.x = TREEV_CAMERA(cam)->target.r * cos( RAD(theta) );
	target.y = TREEV_CAMERA(cam)->target.r * sin( RAD(theta) );
	target.z = TREEV_CAMERA(cam)->target.z;

	/* Absolute camera heading */
	theta = TREEV_CAMERA(cam)->target.theta + cam->theta - 180.0;
	sin_theta = sin( RAD(theta) );
	cos_theta = cos( RAD(theta) );
	sin_phi = sin( RAD(cam->phi) );
	cos_phi = cos( RAD(cam->phi) );

	/* XYZ position */
	xyz_pos.x = target.x + cam->distance * cos_theta * cos_phi;
	xyz_pos.y = target.y + cam->distance * sin_theta * cos_phi;
	xyz_pos.z = target.z + cam->distance * sin_phi;

	/* Convert position from XYZ to RTZ */
	pos->r = XY_LEN(xyz_pos);
	pos->theta = DEG(atan2( xyz_pos.y, xyz_pos.x ));
	pos->z = xyz_pos.z;
}


/* Helper function for camera_look_at_full( ) */
static double
treev_look_at( GNode *node, MorphType mtype, double pan_time_override )
{
	TreeVCamera new_tcam;
	Camera *new_cam;
	RTZvec camera_pos, new_cam_pos;
	double top_dist, height, diameter;
	double alpha;
	double pan_time;
	double k;

	new_cam = CAMERA(&new_tcam);

	/* Construct desired camera state */

	if (geometry_treev_is_leaf( node )) {
		/* Target point */
		TREEV_CAMERA(new_cam)->target.r = geometry_treev_platform_r0( node->parent ) + TREEV_GEOM_PARAMS(node)->leaf.distance;
		TREEV_CAMERA(new_cam)->target.theta = geometry_treev_platform_theta( node->parent ) + TREEV_GEOM_PARAMS(node)->leaf.theta;
		TREEV_CAMERA(new_cam)->target.z = TREEV_GEOM_PARAMS(node->parent)->platform.height + (MAGIC_NUMBER - 1.0) * TREEV_GEOM_PARAMS(node)->leaf.height;

		/* Distance from target point */
		top_dist = 2.5 * field_distance( camera->fov, (SQRT_2 * TREEV_LEAF_NODE_EDGE) );
		new_cam->distance = top_dist + (2.0 - MAGIC_NUMBER) * TREEV_GEOM_PARAMS(node)->leaf.height;

		/* Clipping plane distances */
		new_cam->near_clip = NEAR_TO_DISTANCE_RATIO * top_dist;
		new_cam->far_clip = FAR_TO_NEAR_RATIO * new_cam->near_clip;

		/* Viewing angles */
                new_cam->theta = treev_camera_theta( TREEV_CAMERA(new_cam)->target.theta, node );
		new_cam->phi = 45.0;
		/* Ensure that camera is pitched high enough to see top
		 * and bottom ends of leaf node */
		k = new_cam->distance * sin( RAD(0.25 * camera->fov) ) / ((2.0 - MAGIC_NUMBER) * TREEV_GEOM_PARAMS(node)->leaf.height);
		if ((k >= -1.0) && (k <= 1.0)) {
			alpha = DEG(asin( k )) - 0.25 * camera->fov;
			new_cam->phi = MAX(new_cam->phi, 90.0 - alpha);
		}
	}
	else {
		/* Target point */
		TREEV_CAMERA(new_cam)->target.r = geometry_treev_platform_r0( node ) + 0.3 * TREEV_GEOM_PARAMS(node)->platform.depth - (0.2 * TREEV_PLATFORM_SPACING_DEPTH);
		TREEV_CAMERA(new_cam)->target.theta = geometry_treev_platform_theta( node );
		TREEV_CAMERA(new_cam)->target.z = TREEV_GEOM_PARAMS(node)->platform.height;

		/* Distance from target point */
		height = geometry_treev_max_leaf_height( node );
		diameter = MAX(TREEV_GEOM_PARAMS(node)->platform.depth + (0.5 * TREEV_PLATFORM_SPACING_DEPTH), 0.25 * height);
		new_cam->distance = field_distance( camera->fov, diameter );

		/* Clipping plane distances */
		new_cam->near_clip = NEAR_TO_DISTANCE_RATIO * new_cam->distance;
		new_cam->far_clip = FAR_TO_NEAR_RATIO * new_cam->near_clip;

		/* Viewing angles */
		new_cam->theta = treev_camera_theta( TREEV_CAMERA(new_cam)->target.theta, node );
		new_cam->phi = 30.0;
	}

/* TODO: Implement swing_back for TreeV mode */

	/* Determine how long the camera should take to perform the pan,
	 * if no overriding value was given */
	if (pan_time_override > 0.0)
		pan_time = pan_time_override;
	else {
		treev_get_camera_position( camera, &camera_pos );
		treev_get_camera_position( new_cam, &new_cam_pos );
		k = RTZ_DIST(camera_pos, new_cam_pos) / TREEV_CAMERA_AVG_VELOCITY;
		pan_time = CLAMP(k, TREEV_CAMERA_MIN_PAN_TIME, TREEV_CAMERA_MAX_PAN_TIME);
	}

	/* Get the camera moving */
	morph( &camera->theta, mtype, new_cam->theta, pan_time );
	morph( &camera->phi, mtype, new_cam->phi, pan_time );
	morph( &camera->distance, mtype, new_cam->distance, pan_time );
	morph( &camera->near_clip, mtype, new_cam->near_clip, pan_time );
	morph( &camera->far_clip, mtype, new_cam->far_clip, pan_time );
	morph( &TREEV_CAMERA(camera)->target.r, mtype, TREEV_CAMERA(new_cam)->target.r, pan_time );
	morph( &TREEV_CAMERA(camera)->target.theta, mtype, TREEV_CAMERA(new_cam)->target.theta, pan_time );
	morph( &TREEV_CAMERA(camera)->target.z, mtype, TREEV_CAMERA(new_cam)->target.z, pan_time );

	return pan_time;
}


/* Step callback for camera panning */
static void
pan_step_cb( Morph *unused )
{
	globals.need_redraw = TRUE;
	camera_update_scrollbars( FALSE );
}


/* "Post-callback" for pan_end_cb( ), called exactly one frame later */
static void
post_pan_end( GNode *node )
{
	/* Inform geometry module of camera pan completion */
	geometry_camera_pan_finished( );

	/* Re-enable full user interface */
	window_set_access( TRUE );

	camera_update_scrollbars( TRUE );

	if (node != NULL) {
		/* Show entry for new current node */
		filelist_show_entry( node );
	}
}


/* End callback for camera panning */
static void
pan_end_cb( Morph *morph )
{
	GNode *node;

	globals.need_redraw = TRUE;

	node = (GNode *)morph->data;
	schedule_event( post_pan_end, node, 1 );

	camera_currently_moving = FALSE;
}


/* Shared prologue for any full camera re-pose -- camera_look_at_full( )
 * and, fsn-mode Task C4, camera_warp_to( ) both start here. Claims the
 * camera away from whatever was driving it before this call. */
static void
camera_pan_begin( void )
{
	/* An automatic pan and a flight are two things driving the same
	 * camera variables. The pan wins here, because the user asked for
	 * it (a click on a pedestal, a tree row, Go Back, a warp...) with
	 * the same hands that would otherwise be flying -- the reverse
	 * case, a flight started during a pan, is camera_flight_begin( )'s. */
	camera_flight_end( );

	/* Temporarily disable part of the user interface */
	window_set_access( FALSE );

	if (birdseye_view_active) {
		/* Leave bird's-eye view mode */
		window_birdseye_view_off( );
		birdseye_view_active = FALSE;
	}

	/* Save current scrollbar states */
	prev_scroll_state[X_AXIS] = scroll_state[X_AXIS];
	prev_scroll_state[Y_AXIS] = scroll_state[Y_AXIS];

	/* Halt any ongoing camera pan */
	camera_pan_break( );
}


/* Shared epilogue, paired with camera_pan_begin( ) above: arms the
 * master pan morph and updates history/current-node bookkeeping. `node`
 * is the destination (becomes globals.current_node and the master
 * morph's end-callback data); `pan_time` is whatever the per-mode (or,
 * for camera_warp_to( ), the one fsn-only) pose function returned. */
static void
camera_pan_commit( GNode *node, double pan_time )
{
	GNode *prev_node = NULL;
	boolean backtracking = FALSE;

	/* Master morph */
	camera->pan_part = 0.0;
	morph_full( &camera->pan_part, MORPH_LINEAR, 1.0, pan_time, pan_step_cb, pan_end_cb, node );

	/* Update visited node history */
	if (globals.history != NULL) {
		prev_node = (GNode *)globals.history->data;
		if (prev_node == NULL) {
			/* Camera is backtracking */
			G_LIST_REMOVE(globals.history, NULL);
			backtracking = TRUE;
		}
	}
	if (!backtracking && (node != globals.current_node) && (globals.current_node != prev_node))
		G_LIST_PREPEND(globals.history, globals.current_node);

	/* New current node of interest */
	globals.current_node = node;

	/* Camera is under our control now */
	camera->manual_control = FALSE;

	camera_currently_moving = TRUE;
}


/* fsn-mode, final Milestone C review: FSV_FSN's own drawn/pickable state
 * can disagree with the synchronous dirtree_entry_expanded( ) flag the
 * DEBUG assertions in camera_look_at_full( ) and camera_warp_to( ) below
 * both check. Every other mode's *_draw_recursive( ) (geometry.c) draws a
 * collapsed directory as a single closed folder icon -- nothing under it
 * is drawn or pickable, so the flag and the screen agree by construction.
 * FSN is different on purpose (geometry-fsn-draw.c's fsn_draw_recursive( )/
 * fsn_node_visible( ) comments): a collapsed directory still draws -- and
 * leaves pickable -- its OWN file children on its pedestal; only its
 * subdirectories (and everything under them) are hidden. A plain single
 * click on one of those still-drawn file boxes therefore reaches
 * camera_look_at_full( ) with the file's parent already flagged collapsed.
 * The same gap opens transiently for any FSN node clicked while an
 * ancestor's collapse is still mid-morph: dirtree_entry_expanded( ) flips
 * the instant colexp( ) starts, but the deployment-driven draw/pick
 * recursion only catches up once that ~0.5s/level morph actually finishes
 * -- an overview click (ui_overview.cpp, fsn_layout_nearest( )) or a warp
 * double-click (camera_warp_to( ) below) can land on a still-drawn child
 * in that window just as easily as the steady-state collapsed case above.
 *
 * Fixed with the mechanism this codebase already proves elsewhere for the
 * analogous collapsed-target case -- ui_rail.cpp's Marks panel "Go" button
 * and ui_dialogs.cpp's "Look at target node" (symlink resolution) both
 * expand a collapsed ancestor chain with colexp( COLEXP_EXPAND_ANY ) before
 * panning to it. Centralized here, once, rather than duplicated at every
 * FSN entry path (input.cpp's single-click and warp-lite double-click,
 * ui_overview.cpp's click) -- both functions below already funnel every
 * caller through this point before their own DEBUG assertion, so this is
 * the one place guaranteed to run for all of them, present and future.
 * colexp( COLEXP_EXPAND_ANY ) does not itself call back into
 * camera_look_at_full( )/camera_warp_to( ) (colexp.c's own depth==0
 * epilogue explicitly no-ops the camera for that message -- "something
 * else should already be doing something with the camera"), so this
 * cannot recurse. Scoped to FSV_FSN by an explicit mode check, not left to
 * "the mismatch just never happens elsewhere": every other mode's draw
 * code keeps the invariant true on its own, so this is a deliberate no-op
 * there, not an accidental one. */
static void
fsn_ensure_parent_expanded( GNode *node )
{
	if (globals.fsv_mode != FSV_FSN)
		return;

	if (NODE_IS_DIR(node->parent) && !dirtree_entry_expanded( node->parent ))
		colexp( node->parent, COLEXP_EXPAND_ANY );
}


/* Points the camera at the given node, using the specified motion
 * morph type and (optionally, if value is nonnegative) the specified
 * pan duration */
void
camera_look_at_full( GNode *node, MorphType mtype, double pan_time_override )
{
	double pan_time = 0.0;

	fsn_ensure_parent_expanded( node );

#ifdef DEBUG
	/* Parent directory of target node must be expanded
	 * (or at least be expanding) -- fsn_ensure_parent_expanded( ) above
	 * makes this true rather than merely checking it, for FSV_FSN */
	if (NODE_IS_DIR(node->parent))
		g_assert( dirtree_entry_expanded( node->parent ) );
#endif

	camera_pan_begin( );

	switch (globals.fsv_mode) {
		case FSV_DISCV:
		pan_time = discv_look_at( node, mtype, pan_time_override );
		break;

		case FSV_MAPV:
		pan_time = mapv_look_at( node, mtype, pan_time_override );
		break;

		case FSV_TREEV:
		pan_time = treev_look_at( node, mtype, pan_time_override );
		break;

		case FSV_FSN:
		pan_time = fsn_look_at( node, mtype, pan_time_override );
		break;

		SWITCH_FAIL
	}

	camera_pan_commit( node, pan_time );
}


/* This calls camera_look_at_full( ) with default arguments */
void
camera_look_at( GNode *node )
{
	camera_look_at_full( node, MORPH_SIGMOID, -1.0 );
}


/* Helper function for camera_warp_to( ), fsn-mode Task C4.
 *
 * Shaped exactly like fsn_look_at( ) (this file, above) -- same
 * spherical position math (target + distance * (theta, phi) offset),
 * same near/far clip ratios, same travel-based pan-time formula, same
 * shortest-arc theta unwrap -- but framed to land low and close over
 * `node`'s own pedestal instead of taking in the whole landscape (an
 * ordinary look-at's job). See the FSN_WARP_* note in fsn-style.h. */
static double
fsn_warp_pose( GNode *node, MorphType mtype, double pan_time_override )
{
	MapVCamera new_mcam;
	Camera *new_cam;
	const FsnPedestal *ped;
	XYZvec camera_pos, new_cam_pos, delta;
	XYZvec ext;
	double diameter, pan_time, k;

	new_cam = CAMERA(&new_mcam);

	ped = fsn_layout_get( node );
	if (ped == NULL) {
		/* Same defensive fallback as fsn_look_at( ): should not
		 * happen (FSV_FSN lays out the whole tree), but a pedestal-
		 * less warp target has nowhere to land. Stay put. */
		return FSN_CAMERA_MIN_PAN_TIME;
	}

	/* Target point: the pedestal's own top, raised by
	 * FSN_WARP_HEIGHT_LIFT -- aiming at roughly file-box height rather
	 * than the bare pedestal surface underneath them. Confirmed by an
	 * earlier pass's own screenshot: a target sitting exactly at the
	 * pedestal surface put the camera in the aisle *between* two rows
	 * of boxes, looking down a canyon of their side walls instead of
	 * across their tops. */
	MAPV_CAMERA(new_cam)->target.x = ped->x;
	MAPV_CAMERA(new_cam)->target.y = ped->z;
	MAPV_CAMERA(new_cam)->target.z = ped->h + FSN_WARP_HEIGHT_LIFT;

	/* Same heading as every other FSN pose: the camera always faces
	 * deeper into the tree, warp included. */
	new_cam->theta = FSN_CAMERA_THETA;
	new_cam->phi = FSN_WARP_PHI;

	/* Framed to a *fraction* of the pedestal's own footprint (see
	 * fsn-style.h's FSN_WARP_DIAMETER_FRAC), not the whole thing --
	 * that is what makes the file-box grid fill the view instead of
	 * being seen whole from a diagonal, the "swoop" fsn_look_at( )'s
	 * plain establishing shot does not give. */
	diameter = FSN_WARP_DIAMETER_FRAC * MAX(ped->w, ped->d);
	new_cam->distance = field_distance( camera->fov, MAX(1.0, diameter) );
	new_cam->near_clip = NEAR_TO_DISTANCE_RATIO * new_cam->distance;
	new_cam->far_clip = FAR_TO_NEAR_RATIO * new_cam->near_clip;

	/* Duration: fsn_look_at( )'s own travel-proportional formula,
	 * unchanged -- a warp can be a short hop (already looking at this
	 * pedestal, re-double-clicked) or a long one (from clear across the
	 * landscape), and both should keep feeling like the same gesture. */
	if (pan_time_override > 0.0)
		pan_time = pan_time_override;
	else {
		mapv_get_camera_position( camera, &camera_pos );
		mapv_get_camera_position( new_cam, &new_cam_pos );
		delta.x = new_cam_pos.x - camera_pos.x;
		delta.y = new_cam_pos.y - camera_pos.y;
		delta.z = new_cam_pos.z - camera_pos.z;

		fsn_layout_extents( &ext.x, &ext.y, NULL );
		k = sqrt( XYZ_LEN(delta) / MAX(1.0, hypot( ext.x, ext.y )) );
		pan_time = MAX(FSN_CAMERA_MIN_PAN_TIME,
		    MIN(1.0, k) * FSN_CAMERA_MAX_PAN_TIME);
	}

	/* fsn-mode Task B2 lesson: theta is an angle, morph( ) is not
	 * angle-aware, so a viewer parked just past the wrap point would
	 * otherwise spin the long way round to FSN_CAMERA_THETA. */
	unwrap_theta_toward( new_cam->theta );

	morph( &camera->theta, mtype, new_cam->theta, pan_time );
	morph( &camera->phi, mtype, new_cam->phi, pan_time );
	morph( &camera->distance, mtype, new_cam->distance, pan_time );
	morph( &camera->near_clip, mtype, new_cam->near_clip, pan_time );
	morph( &camera->far_clip, mtype, new_cam->far_clip, pan_time );
	morph( &MAPV_CAMERA(camera)->target.x, mtype, MAPV_CAMERA(new_cam)->target.x, pan_time );
	morph( &MAPV_CAMERA(camera)->target.y, mtype, MAPV_CAMERA(new_cam)->target.y, pan_time );
	morph( &MAPV_CAMERA(camera)->target.z, mtype, MAPV_CAMERA(new_cam)->target.z, pan_time );

	return pan_time;
}


/* fsn-mode Task C4: warp-lite. FSV_FSN's directory double-click, guarded
 * by src/sdl/input.cpp (this function assumes but does not itself check
 * globals.fsv_mode == FSV_FSN -- fsn_warp_pose( ) above reads FSN-only
 * geometry). Mirrors camera_look_at_full( )'s own hook pattern
 * (camera_pan_begin( )/camera_pan_commit( ), factored out above
 * specifically so the two share it) rather than duplicating it, with
 * fsn_warp_pose( ) standing in for that function's per-mode switch --
 * there is only ever one mode here, so there is nothing to switch on. */
void
camera_warp_to( GNode *node )
{
	double pan_time;

	fsn_ensure_parent_expanded( node );

#ifdef DEBUG
	/* Same precondition as camera_look_at_full( )'s (see
	 * fsn_ensure_parent_expanded( )'s own doc comment, above it, for why
	 * this can otherwise be false even for a target that really was
	 * on-screen and pickable): the target's own parent must already be
	 * expanded for it to have been pickable at all. Unlike that
	 * function, `node` itself need not be expanded -- the caller
	 * auto-expands it first only when it was collapsed, but a warp onto
	 * an already-expanded pedestal is exactly the re-double-click case
	 * this task's "no collapse" behavior exists for. */
	if (NODE_IS_DIR(node->parent))
		g_assert( dirtree_entry_expanded( node->parent ) );
#endif

	camera_pan_begin( );

	pan_time = fsn_warp_pose( node, MORPH_SIGMOID, -1.0 );

	camera_pan_commit( node, pan_time );
}


/* Helper function for treev_look_at_lpan( ) */
static void
lpan_stage2( void **data )
{
	GNode *node;
	double pan_time;

	geometry_camera_pan_finished( );

	node = (GNode *)data[0];
	pan_time = *((double *)data[1]);
	xfree( data );

	camera_look_at_full( node, MORPH_SIGMOID, pan_time );
}


/* End callback used by camera_treev_lpan_look_at( ) */
static void
lpan_stage1_end_cb( Morph *morph )
{
	globals.need_redraw = TRUE;
	camera_update_scrollbars( FALSE );

	schedule_event( lpan_stage2, morph->data, 1 );
}


/* This points the camera at the given node, using a two-stage pan in which
 * the camera follows an L-shaped path ("lpan").
 * Note: currently implemented only for TreeV mode, 'cause that's the only
 * mode that uses it */
void
camera_treev_lpan_look_at( GNode *node, double pan_time_override )
{
	static double pan_time;
	TreeVCamera new_tcam;
	Camera *new_cam;
	RTZvec camera_pos, new_cam_pos;
	void **data;

	new_cam = CAMERA(&new_tcam);

	/* Disable part of user interface */
	window_set_access( FALSE );

	if (birdseye_view_active) {
		/* Leave bird's-eye view mode */
		window_birdseye_view_off( );
		birdseye_view_active = FALSE;
	}

	/* Construct desired camera state (stage 1) */

	if (geometry_treev_is_leaf( node )) {
		new_cam->theta = -15.0 * TREEV_GEOM_PARAMS(node)->leaf.theta / TREEV_GEOM_PARAMS(node->parent)->platform.arc_width;
		TREEV_CAMERA(new_cam)->target.r = geometry_treev_platform_r0( node->parent ) + TREEV_GEOM_PARAMS(node)->leaf.distance;
		TREEV_CAMERA(new_cam)->target.theta = geometry_treev_platform_theta( node->parent ) + TREEV_GEOM_PARAMS(node)->leaf.theta;
	}
	else {
		TREEV_CAMERA(new_cam)->target.r = geometry_treev_platform_r0( node ) + (2.0 - MAGIC_NUMBER) * TREEV_GEOM_PARAMS(node)->platform.depth;
		TREEV_CAMERA(new_cam)->target.theta = geometry_treev_platform_theta( node );
		new_cam->theta = -0.125 * (TREEV_CAMERA(new_cam)->target.theta - 90.0);
	}

	/* Duration of pan */
	if (pan_time_override > 0.0)
		pan_time = pan_time_override;
	else {
		treev_get_camera_position( camera, &camera_pos );
		treev_get_camera_position( new_cam, &new_cam_pos );
		pan_time = RTZ_DIST(camera_pos, new_cam_pos) / TREEV_CAMERA_AVG_VELOCITY;
		pan_time = CLAMP(pan_time, TREEV_CAMERA_MIN_PAN_TIME, TREEV_CAMERA_MAX_PAN_TIME);
	}

	camera_pan_break( );

	/* Get the camera moving */
	morph( &camera->theta, MORPH_INV_QUADRATIC, new_cam->theta, pan_time );
	morph( &TREEV_CAMERA(camera)->target.r, MORPH_INV_QUADRATIC, TREEV_CAMERA(new_cam)->target.r, pan_time );
	morph( &TREEV_CAMERA(camera)->target.theta, MORPH_INV_QUADRATIC, TREEV_CAMERA(new_cam)->target.theta, pan_time );

	/* Need to pass along both node and pan_time */
	data = NEW_ARRAY(void *, 2);
	data[0] = node;
	data[1] = &pan_time;
	/* Master morph */
	camera->pan_part = 0.0;
	morph_full( &camera->pan_part, MORPH_LINEAR, 1.0, pan_time, pan_step_cb, lpan_stage1_end_cb, data );

	/* Camera is under our control now */
	camera->manual_control = FALSE;

	camera_currently_moving = TRUE;
}


/* Sends camera back to view the previously visited node */
void
camera_look_at_previous( void )
{
	GNode *prev_node;

	/* Can't backtrack if history is empty */
	if (globals.history == NULL)
		return;

	/* Get previously visited node */
	prev_node = (GNode *)globals.history->data;

	globals.history->data = NULL;
	camera_look_at( prev_node );
}


/* Enters/exits bird's-eye-view mode */
void
camera_birdseye_view( boolean going_up )
{
	union AnyCamera new_anycam;
	Camera *new_cam, *pre_cam;
	XYZvec fsn_ext;
	RTvec ext_c1;
	double pan_time = 0.0;

	new_cam = CAMERA(&new_anycam);
	pre_cam = CAMERA(&pre_birdseye_view_camera);

	/* As in camera_look_at_full( ): an explicit request to re-pose the
	 * camera ends any flight in progress. Doubly so going up, since the
	 * pose saved here as "where the user was" would otherwise keep
	 * drifting after it was saved. */
	camera_flight_end( );

	/* Neutralize user interface */
	window_set_access( FALSE );

	/* Save current scrollbar states */
	prev_scroll_state[X_AXIS] = scroll_state[X_AXIS];
	prev_scroll_state[Y_AXIS] = scroll_state[Y_AXIS];

	/* Halt any ongoing camera pan */
	camera_pan_break( );

	/* Determine length of pan */
	switch (globals.fsv_mode) {
		case FSV_DISCV:
		pan_time = DISCV_CAMERA_MAX_PAN_TIME;
		break;

		case FSV_MAPV:
		pan_time = MAPV_CAMERA_MAX_PAN_TIME;
		break;

		case FSV_TREEV:
		pan_time = TREEV_CAMERA_MAX_PAN_TIME;
		break;

		case FSV_FSN:
		pan_time = FSN_CAMERA_MAX_PAN_TIME;
		break;

		SWITCH_FAIL
	}

	if (going_up) {
		/* Save current camera state */
		memcpy( pre_cam, camera, sizeof(union AnyCamera) );

		/* Build bird's-eye view */
		new_cam->phi = 90.0;
		switch (globals.fsv_mode) {
			case FSV_DISCV:
			new_cam->distance = 2.0 * field_distance( camera->fov, 2.0 * DISCV_GEOM_PARAMS(root_dnode)->radius );
			break;

			case FSV_MAPV:
			new_cam->theta = 270.0;
			new_cam->distance = field_distance( camera->fov, MAPV_NODE_WIDTH(root_dnode) );
			break;

			case FSV_TREEV:
			new_cam->theta = 90.0 - TREEV_CAMERA(camera)->target.theta;
			if (dirtree_entry_expanded( root_dnode )) {
				geometry_treev_get_extents( root_dnode, NULL, &ext_c1 );
				new_cam->distance = field_distance( camera->fov, 2.0 * ext_c1.r );
			}
                        else
				new_cam->distance = 4.0 * camera->distance;
			break;

			case FSV_FSN:
			/* Straight down over the whole landscape. Its extents
			 * come from the FSN layout rather than from
			 * MAPV_NODE_WIDTH( ) as the MapV arm above does -- see
			 * the FSN_CAMERA_* note at the top of this file. */
			new_cam->theta = FSN_CAMERA_THETA;
			fsn_layout_extents( &fsn_ext.x, &fsn_ext.y, NULL );
			new_cam->distance = field_distance( camera->fov,
			    MAX(1.0, MAX(fsn_ext.x, fsn_ext.y)) );
			/* Same reason as fsn_look_at( )'s: this is the other
			 * pan a flight can hand a wrapped heading to */
			unwrap_theta_toward( new_cam->theta );
			break;

			SWITCH_FAIL
		}
		new_cam->near_clip = NEAR_TO_DISTANCE_RATIO * new_cam->distance;
		new_cam->far_clip = FAR_TO_NEAR_RATIO * new_cam->near_clip;

		morph( &camera->theta, MORPH_SIGMOID_ACCEL, new_cam->theta, pan_time );
		morph( &camera->phi, MORPH_SIGMOID_ACCEL, new_cam->phi, pan_time );
		morph( &camera->distance, MORPH_SIGMOID_ACCEL, new_cam->distance, pan_time );
		morph( &camera->near_clip, MORPH_SIGMOID_ACCEL, new_cam->near_clip, pan_time );
		morph( &camera->far_clip, MORPH_SIGMOID_ACCEL, new_cam->far_clip, pan_time );

		birdseye_view_active = TRUE;
	}
	else {
		/* Restore pre-bird's-eye-view camera state */

		/* Third consumer of the same whip fix as fsn_look_at( ) and the
		 * going-up arm above (task-B2-report.md's fix round, item 2):
		 * a flight can leave camera->theta unwrapped past a multiple of
		 * 360, and morph( ) interpolates that raw number rather than
		 * the angle it represents, so restoring straight to
		 * pre_cam->theta can spin most of the way around instead of
		 * taking the short arc back to where the user was before going
		 * up. FSN-only, like the going-up arm's call: DiscV/MapV/TreeV
		 * never wrap theta the way a flight does, so their own
		 * pre-existing "long way round" behavior after a manual
		 * revolve (documented in the same fix-round note) is left
		 * alone here too. */
		if (globals.fsv_mode == FSV_FSN)
			unwrap_theta_toward( pre_cam->theta );
		morph( &camera->theta, MORPH_SIGMOID, pre_cam->theta, pan_time );
		morph( &camera->phi, MORPH_SIGMOID, pre_cam->phi, pan_time );
		morph( &camera->distance, MORPH_SIGMOID, pre_cam->distance, pan_time );
		morph( &camera->near_clip, MORPH_SIGMOID, pre_cam->near_clip, pan_time );
		morph( &camera->far_clip, MORPH_SIGMOID, pre_cam->far_clip, pan_time );

		switch (globals.fsv_mode) {
			case FSV_DISCV:
			morph( &DISCV_CAMERA(camera)->target.x, MORPH_SIGMOID, DISCV_CAMERA(pre_cam)->target.x, pan_time );
			morph( &DISCV_CAMERA(camera)->target.y, MORPH_SIGMOID, DISCV_CAMERA(pre_cam)->target.y, pan_time );
			break;

			case FSV_FSN:
			/* Shares MapV's Cartesian target storage -- see the
			 * FSN_CAMERA_* note at the top of this file */
			case FSV_MAPV:
			morph( &MAPV_CAMERA(camera)->target.x, MORPH_SIGMOID, MAPV_CAMERA(pre_cam)->target.x, pan_time );
			morph( &MAPV_CAMERA(camera)->target.y, MORPH_SIGMOID, MAPV_CAMERA(pre_cam)->target.y, pan_time );
			morph( &MAPV_CAMERA(camera)->target.z, MORPH_SIGMOID, MAPV_CAMERA(pre_cam)->target.z, pan_time );
			break;

			case FSV_TREEV:
			morph( &TREEV_CAMERA(camera)->target.r, MORPH_SIGMOID, TREEV_CAMERA(pre_cam)->target.r, pan_time );
			morph( &TREEV_CAMERA(camera)->target.theta, MORPH_SIGMOID, TREEV_CAMERA(pre_cam)->target.theta, pan_time );
			morph( &TREEV_CAMERA(camera)->target.z, MORPH_SIGMOID, TREEV_CAMERA(pre_cam)->target.z, pan_time );
			break;

			SWITCH_FAIL
		}

		birdseye_view_active = FALSE;
	}

	/* Master morph */
	camera->pan_part = 0.0;
	morph_full( &camera->pan_part, MORPH_LINEAR, 1.0, pan_time, pan_step_cb, pan_end_cb, NULL );

	camera_currently_moving = TRUE;
}


/* Moves camera toward (dk < 0) or away (dk > 0) from view target */
void
camera_dolly( double dk )
{
	camera->distance += (dk * camera->distance / 256.0);
	camera->distance = MAX(camera->distance, 16.0);
	camera->near_clip = NEAR_TO_DISTANCE_RATIO * camera->distance;
	camera->far_clip = FAR_TO_NEAR_RATIO * camera->near_clip;

	/* Camera is under user control */
	camera->manual_control = TRUE;

	camera_update_scrollbars( TRUE );
	redraw( );
}


/* Revolves camera around view target by the given angle deltas */
void
camera_revolve( double dtheta, double dphi )
{
	/* theta = heading, phi = elevation */
	camera->theta -= dtheta;
	camera->phi += dphi;

	/* Keep angles within proper bounds */
	while (camera->theta < 0.0)
		camera->theta += 360.0;
	while (camera->theta > 360.0)
		camera->theta -= 360.0;
	camera->phi = CLAMP(camera->phi, 1.0, 90.0);

	/* Camera is under user control */
	camera->manual_control = TRUE;

	camera_update_scrollbars( TRUE );
	redraw( );
}


/* end camera.c */
