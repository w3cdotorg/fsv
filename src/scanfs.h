/* scanfs.h */

/* Filesystem scanner */

/* fsv - 3D File System Visualizer
 * Copyright (C)1999 Daniel Richard G. <skunk@mit.edu>
 *
 * SPDX-License-Identifier:  LGPL-2.1-or-later
 */


#ifdef FSV_SCANFS_H
	#error
#endif
#define FSV_SCANFS_H


void scanfs( const char *dir );

/* Built-in scan exclusion (see scanfs.c's excluded_dir_names[] doc
 * comment). Default is TRUE. */
void scanfs_set_exclusion( boolean enabled );
boolean scanfs_get_exclusion( void );


/* end scanfs.h */
