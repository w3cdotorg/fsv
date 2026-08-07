/* nvstore.c */

/* Nonvolatile storage library */

/* Copyright (C)1999 Daniel Richard G. <skunk@mit.edu>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/* Was a complete stub ("ALL THIS HAS YET TO BE IMPLEMENTED!") until
 * Task 5.3 of the SDL/Metal port: nvs_open() always returned NULL and
 * every read/write function was a no-op (reads silently fell back to
 * their *_default value; writes vanished). That made color_write_config()/
 * color_read_config() (src/color.c) -- the one and only caller of this
 * whole header -- persist nothing, on *either* frontend, which blocked
 * Task 5.3's "Color Setup survives a relaunch" requirement outright, not
 * just on SDL. This is a real (if minimal) implementation of the exact
 * nvstore.h contract color.c already codes against, so neither color.c
 * nor dialog.c needed a single line changed.
 *
 * Storage model: an in-memory tree of named nodes (NVSNode), each with
 * an optional scalar string value and an ordered list of children --
 * children can repeat the same name, which is what a "vector" (see
 * nvs_vector_begin()/_end()) iterates. `path` arguments are always a
 * single path component here (nvs_change_path()'s real callers in
 * color.c only ever pass a bare key, "..", or "."), so there is no
 * general path-splitting to implement.
 *
 * `current` plus a stack of ancestor frames (`path_stack`) is exactly
 * gnome-config's own "path" cursor idea (this header's key/token
 * naming was clearly modeled on it -- see common.h's leftover
 * gnome_config_get_token() prototype) -- just reimplemented here
 * without linking GConf/gnome-config, neither of which this project
 * otherwise depends on.
 *
 * On-disk format: a tab-indented, one-node-per-line text file --
 * `<tabs><name>[ <value>]`, where indentation depth encodes nesting and
 * repeated sibling names encode a vector, e.g.:
 *
 *   color
 *   	colormode nodetype
 *   	nodetype
 *   		directory #A0A0A0
 *   	wpattern
 *   		group
 *   			color #FF3333
 *   			wp *.c
 *   			wp *.h
 *   		defaultcolor #FFFFA0
 *
 * There is no legacy on-disk format to stay compatible with: this
 * stub never wrote a byte in any released version, so any reasonably
 * simple, round-trippable format is a free choice. Values are escaped
 * (backslash/tab/newline only) so an unusual wildcard pattern can never
 * corrupt the line structure.
 */

#include "nvstore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define xmalloc	malloc
#define xstrdup	strdup
#define xfree	free


/**** In-memory tree ****************/

typedef struct _NVSNode NVSNode;
struct _NVSNode {
	char *name;
	char *value;      /* NULL: no scalar value (a pure "group" node) */
	NVSNode *children; /* first child; siblings link via ->next, insertion order preserved (this is what a vector iterates) */
	NVSNode *next;
};

/* One frame per nvs_change_path() descent still "open" (i.e. not yet
 * balanced by a ".."), so ".." can restore `current` and -- for a
 * descent that consumed a vector slot -- advance that vector's index. */
typedef struct _NVSPathFrame NVSPathFrame;
struct _NVSPathFrame {
	NVSNode *parent;
	NVS_BOOL via_vector;
	NVSPathFrame *up;
};

/* One frame per nvs_vector_begin() still open. Only ever the *top* frame
 * is relevant to any given call: color.c's two vector uses (the
 * wpattern group list, and each group's own pattern list) always nest
 * strictly LIFO, and a vector frame only ever governs calls made while
 * `current` is still the exact node the vector was opened on -- reads/
 * writes/descents for any *other* key at that same node (e.g. a group's
 * own "color" field, read before its inner pattern vector even opens)
 * fall through to the plain, non-vector path below because `current`
 * has moved on by the time that inner vector exists, or because the top
 * frame's node no longer matches. */
typedef struct _NVSVectorFrame NVSVectorFrame;
struct _NVSVectorFrame {
	NVSNode *node;
	int index;
	NVSVectorFrame *up;
};

struct _NVStore {
	char *filename;
	NVSNode *root;    /* synthetic, unnamed -- never itself written out */
	NVSNode *current;
	NVSPathFrame *path_stack;     /* top = head */
	NVSVectorFrame *vector_stack; /* top = head */
};


static NVSNode *
node_new( const char *name )
{
	NVSNode *n = xmalloc( sizeof(NVSNode) );
	n->name = xstrdup( name );
	n->value = NULL;
	n->children = NULL;
	n->next = NULL;
	return n;
}


static void
node_free_recursive( NVSNode *n )
{
	NVSNode *c, *next;

	if (n == NULL)
		return;
	c = n->children;
	while (c != NULL) {
		next = c->next;
		node_free_recursive( c );
		c = next;
	}
	xfree( n->name );
	xfree( n->value );
	xfree( n );
}


/* Frees n's children (recursively) without freeing n itself -- the
 * "keep the current group, clear what's under it" nvs_delete_recursive()
 * needs for color_write_config()'s "wipe, then rewrite" pattern. */
static void
node_clear_children( NVSNode *n )
{
	NVSNode *c, *next;

	c = n->children;
	while (c != NULL) {
		next = c->next;
		node_free_recursive( c );
		c = next;
	}
	n->children = NULL;
}


static void
node_append_child( NVSNode *parent, NVSNode *child )
{
	NVSNode *last;

	if (parent->children == NULL) {
		parent->children = child;
		return;
	}
	last = parent->children;
	while (last->next != NULL)
		last = last->next;
	last->next = child;
}


static NVSNode *
node_find_child( NVSNode *parent, const char *name )
{
	NVSNode *n;

	for (n = parent->children; n != NULL; n = n->next)
		if (!strcmp( n->name, name ))
			return n;
	return NULL;
}


/* The `index`-th (0-based) child named `name`, in sibling order -- what
 * a vector's own index cursor addresses. */
static NVSNode *
node_nth_child_named( NVSNode *parent, const char *name, int index )
{
	NVSNode *n;
	int i = 0;

	for (n = parent->children; n != NULL; n = n->next) {
		if (!strcmp( n->name, name )) {
			if (i == index)
				return n;
			i++;
		}
	}
	return NULL;
}


static int
node_count_children_named( NVSNode *parent, const char *name )
{
	NVSNode *n;
	int count = 0;

	for (n = parent->children; n != NULL; n = n->next)
		if (!strcmp( n->name, name ))
			count++;
	return count;
}


/**** Value escaping ****************/

/* Backslash/tab/newline only -- the sole characters that could corrupt
 * this format's one-node-per-line, tab-indented structure. Every source
 * byte maps to at most two output bytes, so `2*strlen+1` is always a
 * safe upper bound and no realloc is needed. */
static char *
encode_value( const char *v )
{
	char *out = xmalloc( strlen( v ) * 2 + 1 );
	size_t j = 0;
	size_t i;

	for (i = 0; v[i] != '\0'; i++) {
		switch (v[i]) {
			case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
			case '\n': out[j++] = '\\'; out[j++] = 'n'; break;
			case '\t': out[j++] = '\\'; out[j++] = 't'; break;
			default:   out[j++] = v[i]; break;
		}
	}
	out[j] = '\0';
	return out;
}


static char *
decode_value( const char *v )
{
	size_t len = strlen( v );
	char *out = xmalloc( len + 1 );
	size_t j = 0;
	size_t i;

	for (i = 0; i < len; i++) {
		if (v[i] == '\\' && i + 1 < len) {
			i++;
			if (v[i] == 'n')
				out[j++] = '\n';
			else if (v[i] == 't')
				out[j++] = '\t';
			else
				out[j++] = v[i]; /* '\\' -> '\\', and any unrecognized escape */
		} else {
			out[j++] = v[i];
		}
	}
	out[j] = '\0';
	return out;
}


/**** File I/O ****************/

static char *
expand_filename( const char *filename )
{
	const char *home;
	char *out;
	size_t len;

	if (filename[0] != '~' || filename[1] != '/')
		return xstrdup( filename );

	home = getenv( "HOME" );
	if (home == NULL)
		home = "";
	/* filename+1 keeps the leading '/' from "~/...", so this is
	 * exactly home + the rest of filename (minus the '~'). */
	len = strlen( home ) + strlen( filename + 1 );
	out = xmalloc( len + 1 );
	snprintf( out, len + 1, "%s%s", home, filename + 1 );
	return out;
}


static void
write_children( FILE *f, NVSNode *parent, int depth )
{
	NVSNode *n;
	int i;
	char *enc;

	for (n = parent->children; n != NULL; n = n->next) {
		for (i = 0; i < depth; i++)
			fputc( '\t', f );
		fputs( n->name, f );
		if (n->value != NULL) {
			enc = encode_value( n->value );
			fputc( ' ', f );
			fputs( enc, f );
			xfree( enc );
		}
		fputc( '\n', f );
		write_children( f, n, depth + 1 );
	}
}


static NVS_BOOL
save_file( NVStore *nvs )
{
	FILE *f = fopen( nvs->filename, "w" );

	if (f == NULL)
		return 0;
	write_children( f, nvs->root, 0 );
	fclose( f );
	return 1;
}


#define NVS_MAX_DEPTH 64

static void
load_file( NVStore *nvs )
{
	FILE *f = fopen( nvs->filename, "r" );
	NVSNode *stack_at_depth[NVS_MAX_DEPTH];
	char line[4096];

	if (f == NULL)
		return; /* No config yet -- every *_default() call below just
		         * returns its default, same as the old all-stub
		         * behavior for a fresh install. */

	while (fgets( line, sizeof(line), f ) != NULL) {
		size_t len = strlen( line );
		int depth;
		char *rest, *space, *name, *value;
		NVSNode *parent, *node;

		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';
		if (len == 0)
			continue; /* blank line (e.g. trailing newline at EOF) */

		depth = 0;
		while ((size_t)depth < len && line[depth] == '\t')
			depth++;
		if (depth >= NVS_MAX_DEPTH)
			depth = NVS_MAX_DEPTH - 1; /* this writer never nests
			                            * this deep; defends against a
			                            * hand-edited file that does */

		rest = line + depth;
		if (rest[0] == '\0')
			continue; /* a line of pure tabs -- malformed, skip it */

		space = strchr( rest, ' ' );
		value = NULL;
		if (space != NULL) {
			*space = '\0';
			value = decode_value( space + 1 );
		}
		name = rest;
		if (name[0] == '\0') {
			xfree( value );
			continue; /* malformed -- skip rather than crash */
		}

		parent = (depth == 0) ? nvs->root : stack_at_depth[depth - 1];
		node = node_new( name );
		node->value = value; /* already decoded, or NULL */
		node_append_child( parent, node );
		stack_at_depth[depth] = node;
	}
	fclose( f );
}


/**** Public API ****************/

NVStore *
nvs_open( const char *filename )
{
	NVStore *nvs = xmalloc( sizeof(NVStore) );

	nvs->filename = expand_filename( filename );
	nvs->root = node_new( "" );
	nvs->current = nvs->root;
	nvs->path_stack = NULL;
	nvs->vector_stack = NULL;
	load_file( nvs );
	return nvs;
}


NVS_BOOL
nvs_close( NVStore *nvs )
{
	NVS_BOOL ok;
	NVSPathFrame *pf, *pf_up;
	NVSVectorFrame *vf, *vf_up;

	if (nvs == NULL)
		return 0;

	ok = save_file( nvs );

	for (pf = nvs->path_stack; pf != NULL; pf = pf_up) {
		pf_up = pf->up;
		xfree( pf );
	}
	for (vf = nvs->vector_stack; vf != NULL; vf = vf_up) {
		vf_up = vf->up;
		xfree( vf );
	}
	node_free_recursive( nvs->root );
	xfree( nvs->filename );
	xfree( nvs );
	return ok;
}


void
nvs_change_path( NVStore *nvs, const char *path )
{
	NVSPathFrame *f;
	NVSNode *target;
	NVS_BOOL via_vector;

	if (!strcmp( path, "." ))
		return;

	if (!strcmp( path, ".." )) {
		f = nvs->path_stack;
		if (f == NULL)
			return; /* already at the root -- defensive no-op */
		nvs->current = f->parent;
		if (f->via_vector && nvs->vector_stack != NULL)
			nvs->vector_stack->index++;
		nvs->path_stack = f->up;
		xfree( f );
		return;
	}

	/* A descent. If a vector is open on `current` right now, this key
	 * addresses that vector's current slot (creating it, on a write
	 * pass where it doesn't exist yet -- see the file header); otherwise
	 * it is a plain single child, found or created. */
	via_vector = (nvs->vector_stack != NULL && nvs->vector_stack->node == nvs->current);
	if (via_vector)
		target = node_nth_child_named( nvs->current, path, nvs->vector_stack->index );
	else
		target = node_find_child( nvs->current, path );
	if (target == NULL) {
		target = node_new( path );
		node_append_child( nvs->current, target );
	}

	f = xmalloc( sizeof(NVSPathFrame) );
	f->parent = nvs->current;
	f->via_vector = via_vector;
	f->up = nvs->path_stack;
	nvs->path_stack = f;
	nvs->current = target;
}


void
nvs_delete_recursive( NVStore *nvs, const char *path )
{
	NVSNode *target;

	target = !strcmp( path, "." ) ? nvs->current : node_find_child( nvs->current, path );
	if (target != NULL)
		node_clear_children( target );
}


void
nvs_vector_begin( NVStore *nvs )
{
	NVSVectorFrame *f = xmalloc( sizeof(NVSVectorFrame) );

	f->node = nvs->current;
	f->index = 0;
	f->up = nvs->vector_stack;
	nvs->vector_stack = f;
}


void
nvs_vector_end( NVStore *nvs )
{
	NVSVectorFrame *f = nvs->vector_stack;

	if (f == NULL)
		return;
	nvs->vector_stack = f->up;
	xfree( f );
}


NVS_BOOL
nvs_path_present( NVStore *nvs, const char *path )
{
	if (nvs->vector_stack != NULL && nvs->vector_stack->node == nvs->current)
		return node_count_children_named( nvs->current, path ) > nvs->vector_stack->index;
	return node_find_child( nvs->current, path ) != NULL;
}


/* Shared scalar accessors, used by every typed nvs_read_*()/nvs_write_*()
 * below. Vector-aware the same way nvs_change_path() is: a leaf-vector
 * item (color.c's wildcard-pattern list -- repeated *values*, not
 * sub-groups) has no change_path() of its own, so the index has to
 * advance right here instead of on a ".." . */
static const char *
scalar_get( NVStore *nvs, const char *path )
{
	NVSNode *n;

	if (nvs->vector_stack != NULL && nvs->vector_stack->node == nvs->current) {
		n = node_nth_child_named( nvs->current, path, nvs->vector_stack->index );
		nvs->vector_stack->index++;
		return n != NULL ? n->value : NULL;
	}
	n = node_find_child( nvs->current, path );
	return n != NULL ? n->value : NULL;
}


static NVS_BOOL
scalar_present( NVStore *nvs, const char *path )
{
	/* Only used by the *_default() wrappers below, none of which are
	 * ever called while a vector is open in color.c's actual usage --
	 * see the NVSVectorFrame doc comment -- so this deliberately does
	 * not special-case that: nvs_path_present() above is the
	 * vector-aware presence check callers use inside a vector loop. */
	return node_find_child( nvs->current, path ) != NULL;
}


static void
scalar_set( NVStore *nvs, const char *path, const char *value )
{
	NVSNode *n;

	if (nvs->vector_stack != NULL && nvs->vector_stack->node == nvs->current) {
		/* Vector writes always add the next slot -- color_write_config()
		 * always starts from a freshly nvs_delete_recursive()'d group,
		 * so there is never an existing Nth slot to overwrite instead. */
		n = node_new( path );
		node_append_child( nvs->current, n );
		nvs->vector_stack->index++;
	} else {
		n = node_find_child( nvs->current, path );
		if (n == NULL) {
			n = node_new( path );
			node_append_child( nvs->current, n );
		}
	}
	xfree( n->value );
	n->value = xstrdup( value );
}


NVS_BOOL
nvs_read_boolean( NVStore *nvs, const char *path )
{
	const char *v = scalar_get( nvs, path );

	return v != NULL && (!strcmp( v, "true" ) || !strcmp( v, "1" ));
}


int
nvs_read_int( NVStore *nvs, const char *path )
{
	const char *v = scalar_get( nvs, path );

	return v != NULL ? atoi( v ) : 0;
}


int
nvs_read_int_token( NVStore *nvs, const char *path, const char **tokens )
{
	const char *v = scalar_get( nvs, path );
	int i;

	if (v == NULL)
		return 0;
	for (i = 0; tokens[i] != NULL; i++)
		if (!strcmp( tokens[i], v ))
			return i;
	return 0;
}


double
nvs_read_float( NVStore *nvs, const char *path )
{
	const char *v = scalar_get( nvs, path );

	return v != NULL ? atof( v ) : 0.0;
}


char *
nvs_read_string( NVStore *nvs, const char *path )
{
	const char *v = scalar_get( nvs, path );

	return xstrdup( v != NULL ? v : "" );
}


NVS_BOOL
nvs_read_boolean_default( NVStore *nvs, const char *path, NVS_BOOL default_val )
{
	return scalar_present( nvs, path ) ? nvs_read_boolean( nvs, path ) : default_val;
}


int
nvs_read_int_default( NVStore *nvs, const char *path, int default_val )
{
	return scalar_present( nvs, path ) ? nvs_read_int( nvs, path ) : default_val;
}


int
nvs_read_int_token_default( NVStore *nvs, const char *path, const char **tokens, int default_val )
{
	return scalar_present( nvs, path ) ? nvs_read_int_token( nvs, path, tokens ) : default_val;
}


double
nvs_read_float_default( NVStore *nvs, const char *path, double default_val )
{
	return scalar_present( nvs, path ) ? nvs_read_float( nvs, path ) : default_val;
}


char *
nvs_read_string_default( NVStore *nvs, const char *path, const char *default_string )
{
	return scalar_present( nvs, path ) ? nvs_read_string( nvs, path ) : xstrdup( default_string );
}


void
nvs_write_boolean( NVStore *nvs, const char *path, NVS_BOOL val )
{
	scalar_set( nvs, path, val ? "true" : "false" );
}


void
nvs_write_int( NVStore *nvs, const char *path, int val )
{
	char buf[32];

	snprintf( buf, sizeof(buf), "%d", val );
	scalar_set( nvs, path, buf );
}


void
nvs_write_int_token( NVStore *nvs, const char *path, int val, const char **tokens )
{
	scalar_set( nvs, path, tokens[val] );
}


void
nvs_write_float( NVStore *nvs, const char *path, double val )
{
	char buf[64];

	snprintf( buf, sizeof(buf), "%.17g", val );
	scalar_set( nvs, path, buf );
}


void
nvs_write_string( NVStore *nvs, const char *path, const char *string )
{
	scalar_set( nvs, path, string );
}


/* end nvstore.c */
