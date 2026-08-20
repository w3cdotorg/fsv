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

/* Built-in scan exclusion (see scanfs.c's excluded_dir_patterns[] doc
 * comment). Default is TRUE. */
void scanfs_set_exclusion( boolean enabled );
boolean scanfs_get_exclusion( void );

/* Appends a glob pattern (fnmatch(3) syntax) to the exclusion list,
 * on equal footing with the built-ins -- same anchored-basename,
 * directories-only matching, and the same scanfs_set_exclusion( )/
 * FSV_NO_EXCLUDE gates. The string is copied; the caller retains
 * ownership of pattern. */
void scanfs_add_exclude_pattern( const char *pattern );


/* end scanfs.h */
