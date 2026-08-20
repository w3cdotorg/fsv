/* camera.h */

/* Camera control */

/* fsv - 3D File System Visualizer
 * Copyright (C)1999 Daniel Richard G. <skunk@mit.edu>
 *
 * SPDX-License-Identifier:  LGPL-2.1-or-later
 */


#ifdef FSV_CAMERA_H
	#error
#endif
#define FSV_CAMERA_H


/* Standard near-clip-to-camera and far-to-near-clip distance ratios */
#define NEAR_TO_DISTANCE_RATIO	0.5
#define FAR_TO_NEAR_RATIO	128.0


/* Camera type casts */
#define CAMERA(cam)		((Camera *)(cam))
#define DISCV_CAMERA(cam)	((DiscVCamera *)(cam))
#define MAPV_CAMERA(cam)	((MapVCamera *)(cam))
#define TREEV_CAMERA(cam)	((TreeVCamera *)(cam))


/* Base camera definition */
typedef struct _Camera Camera;
struct _Camera {
	double	theta;		/* Heading */
	double	phi;		/* Elevation */
	double	distance;	/* Distance between camera and target */
	double	fov;		/* Field of view, in degrees */
	double	near_clip;	/* Clipping plane distances */
	double	far_clip;
	double	pan_part;	/* Camera pan fraction (always in [0, 1]) */
	boolean	manual_control;	/* TRUE when camera is under manual control */
};

/* DiscV mode camera */
typedef struct _DiscVCamera DiscVCamera;
struct _DiscVCamera {
	Camera	camera;
	XYvec	target;
};

/* MapV mode camera */
typedef struct _MapVCamera MapVCamera;
struct _MapVCamera {
	Camera	camera;
	XYZvec	target;
};

/* TreeV mode camera */
typedef struct _TreeVCamera TreeVCamera;
struct _TreeVCamera {
	Camera	camera;
	RTZvec	target;
};

/* Generalized camera type */
union AnyCamera {
	Camera		camera;
	DiscVCamera	discv_camera;
	MapVCamera	mapv_camera;
	TreeVCamera	treev_camera;
};


/* The camera */
extern Camera *camera;


boolean camera_moving( void );
void camera_init( FsvMode mode, boolean initial_view );
/* Eye position (world x/y ground-plane, z height) for any camera whose
 * mode keys its target off MapVCamera's XYZvec -- MapV itself, and
 * FSV_FSN, which reuses that same union member (see camera.c's own
 * doc comment on this function for the derivation and its other
 * callers). `cam` need not be the global `camera` -- callers morph
 * towards a constructed candidate pose too. */
void camera_ground_position( const Camera *cam, XYZvec *pos );
/* Frontend calls this when the user drags a viewport scrollbar (axis:
 * 0 = x, 1 = y, matching fsv_platform.set_scroll( )/get_scroll( )) */
void camera_scrollbar_moved( int axis );
void camera_update_scrollbars( boolean hard_update );
void camera_pan_finish( void );
void camera_pan_break( void );
#ifdef FSV_ANIMATION_H
void camera_look_at_full( GNode *node, MorphType mtype, double pan_time_override );
#endif
void camera_look_at( GNode *node );
void camera_treev_lpan_look_at( GNode *node, double pan_time_override );
void camera_look_at_previous( void );
void camera_birdseye_view( boolean going_up );
void camera_dolly( double dk );
void camera_revolve( double dtheta, double dphi );

/**** fsn flight navigation (FSV_FSN only -- see camera.c) ****
 *
 * A velocity model, not a morph: there is no destination and no fixed
 * duration, so the camera keeps moving for exactly as long as the user
 * holds the button. The frontend drives it in three parts --
 *
 *   camera_flight_begin( )   on the middle-button press;
 *   camera_flight_update( )  on every motion event while it is held,
 *                            with the pointer's offset FROM THE PRESS
 *                            POINT (not a per-event delta), in the same
 *                            pixel space the frontend's other gestures
 *                            use; `vertical` (Shift) remaps the y offset
 *                            from forward speed to climb rate;
 *   camera_flight_end( )     on release, and on anything else that takes
 *                            the camera away from the user.
 *
 * -- plus camera_flight_tick( ), which the frontend's main loop calls
 * once per iteration (next to fsv_animation_tick( )) to integrate the
 * current rates over real elapsed time. It is a cheap no-op when no
 * flight is in progress, so it is safe to call unconditionally.
 *
 * Deviation from the task brief's sketched interface: begin( ) takes no
 * press coordinates. The frontend already owns "where the pointer is"
 * (src/sdl/input.cpp's g_prev_x/g_prev_y and friends) and passes an
 * offset to update( ) anyway, so a second copy of the press point in
 * here would be state that can only ever disagree.
 *
 * All four are safe to call from any mode: begin( ) declines outside
 * FSV_FSN and the rest are then no-ops. */
void camera_flight_begin( void );
void camera_flight_update( double dx_from_press, double dy_from_press, boolean vertical );
void camera_flight_end( void );
void camera_flight_tick( void );
boolean camera_flight_active( void );


/**** fsn warp-lite (FSV_FSN only -- see camera.c, Task C4) ****
 *
 * Upstream fsn's directory "warp", scoped down: flies the camera onto
 * `node`'s own pedestal, landing low and close over it (file boxes
 * filling the frame) rather than the wide establishing shot
 * camera_look_at( ) gives a directory. Shares camera_look_at_full( )'s
 * hook pattern (ends a flight, breaks any pan in progress, disables
 * access for the duration, pushes history) -- see its own doc comment --
 * but takes no MorphType/pan_time_override: every caller wants the same
 * MORPH_SIGMOID landing, so there is nothing for a caller to override.
 *
 * `node` must already be a directory with FSN geometry (fsn_layout_get( )
 * returning non-NULL); the caller -- src/sdl/input.cpp's FSN-mode
 * double-click branch -- is the only one, and it auto-expands a
 * collapsed target *before* calling this, so the deployment morph runs
 * during the fly-in rather than snapping in ahead of it. */
void camera_warp_to( GNode *node );


/* end camera.h */
