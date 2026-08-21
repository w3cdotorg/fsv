/* fscache.c */

/* Scan cache -- see fscache.h and
 * docs/superpowers/specs/2026-08-21-scan-cache-design.md */

/* fsv - 3D File System Visualizer
 * Copyright (C)1999 Daniel Richard G. <skunk@mit.edu>
 * Copyright (C) 2021 Janne Blomqvist <blomqvist.janne@gmail.com>
 *
 * SPDX-License-Identifier:  LGPL-2.1-or-later
 */


#include "common.h"
#include "fscache.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "scanfs.h" /* scanfs_exclusion_fingerprint( ) */


/* On-disk format (native-endian -- a cache is a per-machine artifact,
 * discarded wholesale on any mismatch):
 *
 *   header:  magic "FSVC" | u32 version | u32 endian (0x01020304)
 *            | i64 scan_time | u32 root_len | root bytes
 *            | u32 fingerprint_len | fingerprint bytes
 *   records: depth-first from the root directory --
 *            u8 type | i64 size | i64 size_alloc | u32 uid | u32 gid
 *            | i64 atime | i64 mtime | i64 ctime
 *            | u16 name_len | name bytes
 *            | (directories only) u32 n_children | children records
 *
 * The root directory's own record carries its scan-time stat scalars
 * like any other; its name is the root's basename (never consulted on
 * load -- the header's full path is the identity). */

#define FSCACHE_MAGIC   "FSVC"
#define FSCACHE_VERSION 1
#define FSCACHE_ENDIAN  0x01020304u

/* Same-second race guard (see fscache_dir_matches( )) */
#define FSCACHE_MTIME_SLACK 1


struct _FscacheDir {
	time_t		mtime;
	time_t		ctime;
	int		n_entries;
	FscacheEntry	*entries;
	GHashTable	*by_name; /* name -> FscacheEntry*, keys point into entries */
};


static boolean fscache_enabled = TRUE;
static boolean skip_next_load = FALSE;

/* The loaded index. `image` is the raw file contents; every entry name
 * points into it. `dirs`/`entries` are flat arrays owning the parsed
 * structs (children reference them by pointer). */
static char *image = NULL;
static gsize image_len = 0;
static FscacheDir *root_dir = NULL;
static GPtrArray *all_dirs = NULL; /* FscacheDir*, for teardown */
static time_t loaded_scan_time = 0;

static int replayed_dirs = 0;


void
fscache_set_enabled( boolean enabled )
{
	fscache_enabled = enabled;
}


boolean
fscache_get_enabled( void )
{
	if (g_getenv( "FSV_NO_CACHE" ) != NULL)
		return FALSE;
	return fscache_enabled;
}


void
fscache_skip_next_load( void )
{
	skip_next_load = TRUE;
}


/* Cache file path for a scanned root: $FSV_CACHE_DIR (tests) or the
 * platform cache dir + "/fsv", file named by the upstream-1999 ticket's
 * own convention -- the absolute path with '/' replaced by '@'
 * ("@usr@lib"). Caller frees. `mkdirs` creates the directory (save
 * side); the load side leaves the filesystem untouched. */
static char *
cache_file_path( const char *abs_root, boolean mkdirs )
{
	const char *override = g_getenv( "FSV_CACHE_DIR" );
	char *dir, *fname, *path;
	int i;

	if (override != NULL)
		dir = g_strdup( override );
	else
		dir = g_build_filename( g_get_user_cache_dir( ), "fsv", NULL );

	if (mkdirs)
		g_mkdir_with_parents( dir, 0755 );

	fname = g_strdup( abs_root );
	for (i = 0; fname[i] != '\0'; i++)
		if (fname[i] == G_DIR_SEPARATOR)
			fname[i] = '@';

	path = g_build_filename( dir, fname, NULL );
	g_free( dir );
	g_free( fname );

	return path;
}


/* ---- Load side ------------------------------------------------------ */

/* Bounded reader over the loaded image */
typedef struct {
	const char *p;
	gsize left;
	boolean ok;
} Reader;

static void
read_bytes( Reader *r, void *out, gsize n )
{
	if (!r->ok || r->left < n) {
		r->ok = FALSE;
		return;
	}
	memcpy( out, r->p, n );
	r->p += n;
	r->left -= n;
}

static guint32
read_u32( Reader *r )
{
	guint32 v = 0;
	read_bytes( r, &v, sizeof(v) );
	return v;
}

static gint64
read_i64( Reader *r )
{
	gint64 v = 0;
	read_bytes( r, &v, sizeof(v) );
	return v;
}

/* Returns a pointer into the image (NUL termination is the writer's
 * job -- see write_str( )), or NULL on truncation. */
static const char *
read_str( Reader *r, gsize len )
{
	const char *s;

	if (!r->ok || r->left < len + 1) {
		r->ok = FALSE;
		return NULL;
	}
	s = r->p;
	if (s[len] != '\0') {
		r->ok = FALSE;
		return NULL;
	}
	r->p += len + 1;
	r->left -= len + 1;
	return s;
}

/* Parse one directory's children (n_children already consumed by the
 * caller for the root; recursive for subdirectories). Returns NULL on
 * any anomaly. Depth guard: a filesystem 512 deep is not a cache this
 * code should be trusting. */
static FscacheDir *
parse_dir( Reader *r, guint32 n_children, int depth )
{
	FscacheDir *dir;
	guint32 i;

	if (!r->ok || depth > 512 || n_children > 16u * 1024u * 1024u) {
		r->ok = FALSE;
		return NULL;
	}

	dir = g_new0( FscacheDir, 1 );
	g_ptr_array_add( all_dirs, dir );
	dir->n_entries = (int)n_children;
	dir->entries = g_new0( FscacheEntry, MAX(n_children, 1u) );
	dir->by_name = g_hash_table_new( g_str_hash, g_str_equal );

	for (i = 0; r->ok && i < n_children; i++) {
		FscacheEntry *ent = &dir->entries[i];
		guint8 type8 = 0;
		guint16 name_len = 0;
		guint32 uid = 0, gid = 0;

		read_bytes( r, &type8, sizeof(type8) );
		ent->type = (NodeType)type8;
		ent->size = read_i64( r );
		ent->size_alloc = read_i64( r );
		read_bytes( r, &uid, sizeof(uid) );
		read_bytes( r, &gid, sizeof(gid) );
		ent->user_id = (uid_t)uid;
		ent->group_id = (gid_t)gid;
		ent->atime = (time_t)read_i64( r );
		ent->mtime = (time_t)read_i64( r );
		ent->ctime = (time_t)read_i64( r );
		read_bytes( r, &name_len, sizeof(name_len) );
		ent->name = read_str( r, name_len );
		if (!r->ok || ent->name == NULL || ent->name[0] == '\0')
			r->ok = FALSE;
		else if (ent->type >= NUM_NODE_TYPES)
			r->ok = FALSE;
		else if (ent->type == NODE_DIRECTORY) {
			guint32 sub_n = read_u32( r );
			FscacheDir *sub = parse_dir( r, sub_n, depth + 1 );
			if (sub != NULL) {
				sub->mtime = ent->mtime;
				sub->ctime = ent->ctime;
			}
			ent->subdir = sub;
		}
		if (r->ok)
			g_hash_table_insert( dir->by_name, (gpointer)ent->name, ent );
	}

	return r->ok ? dir : NULL;
}


void
fscache_release( void )
{
	int i;

	if (all_dirs != NULL) {
		for (i = 0; i < (int)all_dirs->len; i++) {
			FscacheDir *dir = (FscacheDir *)g_ptr_array_index( all_dirs, i );
			g_hash_table_destroy( dir->by_name );
			g_free( dir->entries );
			g_free( dir );
		}
		g_ptr_array_free( all_dirs, TRUE );
		all_dirs = NULL;
	}
	root_dir = NULL;
	g_free( image );
	image = NULL;
	image_len = 0;
	/* loaded_scan_time and replayed_dirs deliberately survive: they
	 * describe the scan that just consumed this index (the "since
	 * previous scan" color anchor and the tests both read them after
	 * scanfs( ) has released the index). fscache_load( ) resets both. */
}


boolean
fscache_load( const char *abs_root )
{
	char *path, *fingerprint = NULL;
	char magic[4];
	Reader r;
	guint32 version, endian, len;
	const char *stored_root, *stored_fp;
	gint64 scan_time;
	guint32 root_children;
	guint8 root_type = 0;
	FscacheEntry root_ent;
	boolean ok = FALSE;

	fscache_release( );
	loaded_scan_time = 0;
	replayed_dirs = 0;

	if (!fscache_get_enabled( ))
		return FALSE;
	if (skip_next_load) {
		skip_next_load = FALSE;
		return FALSE;
	}

	path = cache_file_path( abs_root, FALSE );
	if (!g_file_get_contents( path, &image, &image_len, NULL )) {
		g_free( path );
		image = NULL;
		return FALSE;
	}
	g_free( path );

	r.p = image;
	r.left = image_len;
	r.ok = TRUE;

	read_bytes( &r, magic, 4 );
	version = read_u32( &r );
	endian = read_u32( &r );
	scan_time = read_i64( &r );
	if (!r.ok || memcmp( magic, FSCACHE_MAGIC, 4 ) != 0 ||
	    version != FSCACHE_VERSION || endian != FSCACHE_ENDIAN)
		goto out;

	len = read_u32( &r );
	stored_root = read_str( &r, len );
	if (stored_root == NULL || strcmp( stored_root, abs_root ) != 0)
		goto out;

	len = read_u32( &r );
	stored_fp = read_str( &r, len );
	fingerprint = scanfs_exclusion_fingerprint( );
	if (stored_fp == NULL || strcmp( stored_fp, fingerprint ) != 0)
		goto out;

	/* Root record: same shape as any entry (parse it via a one-entry
	 * scratch, then hang on to its subtree) */
	memset( &root_ent, 0, sizeof(root_ent) );
	read_bytes( &r, &root_type, sizeof(root_type) );
	if (root_type != NODE_DIRECTORY)
		r.ok = FALSE;
	root_ent.size = read_i64( &r );
	root_ent.size_alloc = read_i64( &r );
	{
		guint32 uid = 0, gid = 0;
		guint16 name_len = 0;
		read_bytes( &r, &uid, sizeof(uid) );
		read_bytes( &r, &gid, sizeof(gid) );
		root_ent.atime = (time_t)read_i64( &r );
		root_ent.mtime = (time_t)read_i64( &r );
		root_ent.ctime = (time_t)read_i64( &r );
		read_bytes( &r, &name_len, sizeof(name_len) );
		root_ent.name = read_str( &r, name_len );
	}
	if (!r.ok || root_ent.name == NULL)
		goto out;

	all_dirs = g_ptr_array_new( );
	root_children = read_u32( &r );
	root_dir = parse_dir( &r, root_children, 0 );
	if (root_dir == NULL || r.left != 0)
		goto out;
	root_dir->mtime = root_ent.mtime;
	root_dir->ctime = root_ent.ctime;

	loaded_scan_time = (time_t)scan_time;
	ok = TRUE;

out:
	g_free( fingerprint );
	if (!ok)
		fscache_release( );
	return ok;
}


time_t
fscache_prev_scan_time( void )
{
	return loaded_scan_time;
}


const FscacheDir *
fscache_root( void )
{
	return root_dir;
}


const FscacheEntry *
fscache_dir_entry_by_name( const FscacheDir *dir, const char *name )
{
	if (dir == NULL)
		return NULL;
	return (const FscacheEntry *)g_hash_table_lookup( dir->by_name, name );
}


const FscacheDir *
fscache_dir_child( const FscacheDir *dir, const char *name )
{
	const FscacheEntry *ent = fscache_dir_entry_by_name( dir, name );

	return ent == NULL ? NULL : ent->subdir;
}


int
fscache_dir_entry_count( const FscacheDir *dir )
{
	return dir == NULL ? 0 : dir->n_entries;
}


const FscacheEntry *
fscache_dir_entry( const FscacheDir *dir, int i )
{
	if (dir == NULL || i < 0 || i >= dir->n_entries)
		return NULL;
	return &dir->entries[i];
}


boolean
fscache_dir_matches( const FscacheDir *dir, time_t mtime, time_t ctime )
{
	if (dir == NULL)
		return FALSE;

	/* Same-second race guard: a directory whose entries changed in the
	 * same second the cache was written could change AGAIN in that
	 * second without moving its mtime -- distrust anything cached that
	 * close to the snapshot (the git index's classic problem). mtime
	 * only: ctime races can't hide entry-set changes (those always move
	 * mtime too), and guarding ctime would spuriously distrust every
	 * directory whose metadata was touched near scan time. */
	if (dir->mtime >= loaded_scan_time - FSCACHE_MTIME_SLACK)
		return FALSE;

	return dir->mtime == mtime && dir->ctime == ctime;
}


int
fscache_replayed_dirs( void )
{
	return replayed_dirs;
}


void
fscache_count_replayed_dir( void )
{
	++replayed_dirs;
}


/* ---- Save side ------------------------------------------------------ */

static void
write_u32( FILE *fp, guint32 v )
{
	fwrite( &v, sizeof(v), 1, fp );
}

static void
write_i64( FILE *fp, gint64 v )
{
	fwrite( &v, sizeof(v), 1, fp );
}

/* Length-prefix elsewhere; the string itself is written NUL-included so
 * the loader can point directly into the image. */
static void
write_str_bytes( FILE *fp, const char *s )
{
	fwrite( s, strlen( s ) + 1, 1, fp );
}

static int
count_children( GNode *dnode )
{
	GNode *node;
	int n = 0;

	for (node = dnode->children; node != NULL; node = node->next)
		++n;
	return n;
}

static void
write_node( FILE *fp, GNode *node )
{
	const NodeDesc *desc = NODE_DESC(node);
	guint8 type8 = (guint8)desc->type;
	guint16 name_len = (guint16)strlen( desc->name );
	guint32 uid = (guint32)desc->user_id, gid = (guint32)desc->group_id;

	fwrite( &type8, sizeof(type8), 1, fp );
	write_i64( fp, desc->size );
	write_i64( fp, desc->size_alloc );
	fwrite( &uid, sizeof(uid), 1, fp );
	fwrite( &gid, sizeof(gid), 1, fp );
	write_i64( fp, (gint64)desc->atime );
	write_i64( fp, (gint64)desc->mtime );
	write_i64( fp, (gint64)desc->ctime );
	fwrite( &name_len, sizeof(name_len), 1, fp );
	write_str_bytes( fp, desc->name );

	if (NODE_IS_DIR(node)) {
		GNode *child;

		write_u32( fp, (guint32)count_children( node ) );
		for (child = node->children; child != NULL; child = child->next)
			write_node( fp, child );
	}
}


void
fscache_save( GNode *root_node, const char *abs_root )
{
	char *path, *tmp_path, *fingerprint;
	FILE *fp;

	if (!fscache_get_enabled( ))
		return;
	if (root_node == NULL || !NODE_IS_DIR(root_node))
		return;

	/* A node name longer than a u16 can't be encoded; no filesystem
	 * allows one (NAME_MAX is 255 everywhere this runs), so this is a
	 * belt-and-suspenders skip of the whole save, not a per-node one. */

	path = cache_file_path( abs_root, TRUE );
	tmp_path = g_strconcat( path, ".tmp", NULL );

	fp = fopen( tmp_path, "wb" );
	if (fp == NULL) {
		g_message( "fscache: cannot write %s: %s", tmp_path, g_strerror( errno ) );
		g_free( path );
		g_free( tmp_path );
		return;
	}

	fwrite( FSCACHE_MAGIC, 4, 1, fp );
	write_u32( fp, FSCACHE_VERSION );
	write_u32( fp, FSCACHE_ENDIAN );
	write_i64( fp, (gint64)time( NULL ) );
	write_u32( fp, (guint32)strlen( abs_root ) );
	write_str_bytes( fp, abs_root );
	fingerprint = scanfs_exclusion_fingerprint( );
	write_u32( fp, (guint32)strlen( fingerprint ) );
	write_str_bytes( fp, fingerprint );
	g_free( fingerprint );

	write_node( fp, root_node );

	if (fclose( fp ) != 0 || rename( tmp_path, path ) != 0) {
		g_message( "fscache: cannot finalize %s: %s", path, g_strerror( errno ) );
		unlink( tmp_path );
	}

	g_free( path );
	g_free( tmp_path );
}


/* end fscache.c */
