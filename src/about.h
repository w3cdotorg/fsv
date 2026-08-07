/* about.h */

/* Help -> About... */

/* fsv - 3D File System Visualizer
 * Copyright (C)1999 Daniel Richard G. <skunk@mit.edu>
 *
 * SPDX-License-Identifier:  LGPL-2.1-or-later
 */


#ifdef FSV_ABOUT_H
	#error
#endif
#define FSV_ABOUT_H


typedef enum {
	ABOUT_BEGIN,
	ABOUT_END,
	ABOUT_DRAW,
	ABOUT_CHECK
} AboutMesg;


boolean about( AboutMesg mesg );

/* Draws the splash screen (the "fsv" logo plus its captions).
 *
 * This lives with the About presentation rather than in geometry.c: both
 * draw the same 3D "fsv" letters through the same dedicated shader
 * program (per-vertex color + linear fog, src/fsv-about-*.glsl), which is
 * a separate pipeline from the scene shader geometry.c draws everything
 * else with. Frontends that have no About/splash screen implement this as
 * a no-op. */
void about_splash_draw( void );


/* end about.h */
