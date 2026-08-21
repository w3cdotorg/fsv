/* fscache.h */

/* Scan cache -- persists the scanned tree's per-node stat scalars so the
 * next scan of the same root can replay unchanged directories instead of
 * scandir()+lstat()ing them again.
 *
 * Design: docs/superpowers/specs/2026-08-21-scan-cache-design.md
 * (upstream-1999 TODO "scan caching"). The semantics worth restating at
 * the API boundary: a directory REPLAYS when its fresh lstat mtime+ctime
 * equal the cached ones -- its files then come straight from the cache
 * (no lstat), while its subdirectories are always lstat()ed fresh (a
 * directory's mtime says nothing about its descendants). An in-place
 * file edit inside an unchanged directory is therefore invisible until
 * the directory itself changes or the caller skips the cache
 * (fscache_skip_next_load( ), the File->Rescan path). */

/* fsv - 3D File System Visualizer
 * Copyright (C)1999 Daniel Richard G. <skunk@mit.edu>
 * Copyright (C) 2021 Janne Blomqvist <blomqvist.janne@gmail.com>
 *
 * SPDX-License-Identifier:  LGPL-2.1-or-later
 */

#ifdef FSV_FSCACHE_H
	#error
#endif
#define FSV_FSCACHE_H


typedef struct _FscacheDir FscacheDir;

/* One cached directory entry: the NodeDesc scalars stat_node( ) would
 * have produced, plus the subtree handle when the entry is itself a
 * directory (NULL for leaves). `name` points into the loaded cache
 * image and lives until fscache_release( ). */
typedef struct _FscacheEntry FscacheEntry;
struct _FscacheEntry {
	const char	*name;
	NodeType	type;
	int64		size;
	int64		size_alloc;
	uid_t		user_id;
	gid_t		group_id;
	time_t		atime;
	time_t		mtime;
	time_t		ctime;
	const FscacheDir *subdir; /* non-NULL iff type == NODE_DIRECTORY */
};

/* Master switch (--no-cache / FSV_NO_CACHE env). Default on. */
void fscache_set_enabled( boolean enabled );
boolean fscache_get_enabled( void );

/* One-shot read-side bypass: the next fscache_load( ) fails as if no
 * cache existed (File->Rescan wants disk truth), the save side is
 * unaffected. */
void fscache_skip_next_load( void );

/* Load the cache for `abs_root` (resolved absolute path). Returns TRUE
 * and installs the index on success; FALSE (and no index) on any
 * mismatch -- missing/corrupt file, other root, other exclusion
 * fingerprint, disabled, or a pending skip. */
boolean fscache_load( const char *abs_root );

/* Wall-clock time of the scan that wrote the cache consumed by the
 * current/most recent scan; 0 when that scan had no usable cache.
 * Deliberately survives fscache_release( ) -- the ColorByTimestamp
 * "since previous scan" anchor reads it after the scan completes. Reset
 * by the next fscache_load( ). */
time_t fscache_prev_scan_time( void );

/* The loaded index. fscache_root( ) is the scanned root directory (NULL
 * when nothing is loaded). Lookup is by byte-exact entry name;
 * fscache_dir_child( ) shortcuts to the named entry's subtree (NULL for
 * absent entries and non-directories alike). All accept a NULL dir. */
const FscacheDir *fscache_root( void );
const FscacheEntry *fscache_dir_entry_by_name( const FscacheDir *dir, const char *name );
const FscacheDir *fscache_dir_child( const FscacheDir *dir, const char *name );
int fscache_dir_entry_count( const FscacheDir *dir );
const FscacheEntry *fscache_dir_entry( const FscacheDir *dir, int i );

/* TRUE when a fresh lstat of the directory matches the cached snapshot
 * closely enough to replay its contents. Also applies the same-second
 * race guard (see the .c). */
boolean fscache_dir_matches( const FscacheDir *dir, time_t mtime, time_t ctime );

/* Serialize the finished tree (root_dnode and down) for `abs_root`.
 * Best effort: failures log and return. No-op when disabled. */
void fscache_save( GNode *root_node, const char *abs_root );

/* Drop the loaded index (if any). Safe to call when nothing is loaded. */
void fscache_release( void );

/* Number of directories whose contents were replayed from the cache
 * during the current/most recent scan. Reset by fscache_load( ) and by
 * fscache_release( ). Observability + tests. */
int fscache_replayed_dirs( void );
void fscache_count_replayed_dir( void ); /* scanfs.c's replay path */


/* end fscache.h */
