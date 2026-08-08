/* geometry-fsn.c */

/* FSV_FSN mode: pedestals on a ground plane, connected by wires --
 * the landscape of the original SGI fsn (see task-B1-brief.md's
 * reference screenshots).
 *
 * The module is split in two halves, deliberately and strictly:
 *
 *   LAYOUT  fsn_geometry_init( ) and everything it calls is pure math.
 *           It reads the scanned tree and writes per-node geometry into
 *           the node descriptors' own scratch space; it makes no gpu.h
 *           call of any kind, so it can be exercised headless
 *           (tests/test_fsn_layout.c).
 *   DRAW    fsn_geometry_draw( ) turns that geometry into gpu.h calls.
 *
 * fsv - 3D File System Visualizer
 * SPDX-License-Identifier:  LGPL-2.1-or-later
 */


#include "common.h"
#include "geometry-fsn.h"

#include "animation.h"	/* morph_break( ) */
#include "dirtree.h"	/* dirtree_entry_expanded( ) */
#include "fsn-style.h"
#include "geometry.h"	/* geometry_queue_rebuild( ) */


/* Per-directory scratch beyond the five doubles FsnPedestal already
 * claims in NodeDesc::geomparams. DirNodeDesc::geomparams2 is three
 * more doubles, which is exactly what the layout needs to carry from its
 * measuring pass into its placement pass. (TreeV uses the same eight
 * doubles as one contiguous struct; FSN keeps them as two, because only
 * directories have the second half.) */
#define FSN_DIR_SPAN(dnode)	(DIR_NODE_DESC(dnode)->geomparams2[0])
#define FSN_DIR_COLS(dnode)	(DIR_NODE_DESC(dnode)->geomparams2[1])
#define FSN_DIR_ROWS(dnode)	(DIR_NODE_DESC(dnode)->geomparams2[2])


/* Root of the current layout, or NULL when there is none. Doubles as the
 * "is the geometry in the node descriptors ours?" flag -- geomparams is
 * shared scratch space that whichever mode ran last owns. */
static GNode *fsn_root = NULL;

/* Bounding box of the placed landscape (see fsn_layout_extents( )) */
static double fsn_extent_w = 0.0;
static double fsn_extent_d = 0.0;
static double fsn_extent_h = 0.0;

/* Accumulators used while placing; folded into the three above at the
 * end of fsn_geometry_init( ) */
static double fsn_min_x, fsn_max_x, fsn_min_y, fsn_max_y;


/**** LAYOUT **************************************************/


/* Maps a byte count onto a height, logarithmically and with a ceiling.
 * See the FSN_*_H_* constants in fsn-style.h for why log2. */
static double
fsn_size_height( int64 size, double h_min, double h_scale, double h_max )
{
	double bytes, k;

	bytes = (size > 0) ? (double)size : 0.0;
	k = log2( 1.0 + bytes / FSN_SIZE_UNIT );

	return MIN(h_max, h_min + h_scale * k);
}


/* Measuring pass: bottom-up. Assigns every node its footprint and height,
 * and every directory the ground width its whole subtree needs, without
 * committing to any position yet -- a directory's width depends on its
 * descendants, so nothing can be placed until all of them are measured. */
static void
fsn_measure( GNode *dnode )
{
	FsnPedestal *ped, *box;
	GNode *node;
	int nfiles = 0, ndirs = 0;
	int cols, rows;
	double grid_w, grid_d, span = 0.0;

	g_assert( NODE_IS_DIR(dnode) );

	/* Bring deployment in line with the directory tree, exactly as
	 * mapv_init_recursive( ) does. The layout itself is deployment-
	 * independent (a pedestal never moves), but the draw pass and
	 * colexp.c both need the flag to start out agreeing with the
	 * frontend's expand/collapse state. */
	morph_break( &DIR_NODE_DESC(dnode)->deployment );
	DIR_NODE_DESC(dnode)->deployment =
	    dirtree_entry_expanded( dnode ) ? 1.0 : 0.0;

	for (node = dnode->children; node != NULL; node = node->next) {
		if (NODE_IS_DIR(node))
			++ndirs;
		else
			++nfiles;
	}

	/* The files go on the pedestal top in a squarish row-major grid,
	 * which is what dictates the pedestal's own footprint */
	cols = (nfiles > 0) ? (int)ceil( sqrt( (double)nfiles ) ) : 0;
	rows = (cols > 0) ? ((nfiles + cols - 1) / cols) : 0;
	FSN_DIR_COLS(dnode) = (double)cols;
	FSN_DIR_ROWS(dnode) = (double)rows;

	grid_w = (cols > 0) ? (cols * FSN_BOX_EDGE + (cols - 1) * FSN_BOX_GAP) : 0.0;
	grid_d = (rows > 0) ? (rows * FSN_BOX_EDGE + (rows - 1) * FSN_BOX_GAP) : 0.0;

	ped = FSN_GEOM_PARAMS(dnode);
	ped->w = MAX(FSN_PEDESTAL_MIN_EDGE, grid_w + 2.0 * FSN_PEDESTAL_MARGIN);
	ped->d = MAX(FSN_PEDESTAL_MIN_EDGE, grid_d + 2.0 * FSN_PEDESTAL_MARGIN);
	ped->h = fsn_size_height( DIR_NODE_DESC(dnode)->subtree.size,
	    FSN_PEDESTAL_H_MIN, FSN_PEDESTAL_H_SCALE, FSN_PEDESTAL_H_MAX );

	for (node = dnode->children; node != NULL; node = node->next) {
		if (NODE_IS_DIR(node)) {
			/* Depth-first: a child's span must exist before this
			 * directory's own span can be totalled up below */
			fsn_measure( node );
			span += FSN_DIR_SPAN(node);
			continue;
		}

		/* Every file box has the same footprint; only its height
		 * carries its size (see fsn-style.h). Position comes later,
		 * in fsn_place( ), once the pedestal has one. */
		box = FSN_GEOM_PARAMS(node);
		box->w = FSN_BOX_EDGE;
		box->d = FSN_BOX_EDGE;
		box->h = fsn_size_height( NODE_DESC(node)->size,
		    FSN_BOX_H_MIN, FSN_BOX_H_SCALE, FSN_BOX_H_MAX );
	}

	if (ndirs > 1)
		span += (double)(ndirs - 1) * FSN_SIBLING_GAP;

	/* A subtree is at least as wide as the pedestal at its head */
	FSN_DIR_SPAN(dnode) = MAX(ped->w, span);
}


/* Grows the ground-plane half of the landscape's bounding box */
static void
fsn_note_footprint( double x0, double x1, double y0, double y1 )
{
	fsn_min_x = MIN(fsn_min_x, x0);
	fsn_max_x = MAX(fsn_max_x, x1);
	fsn_min_y = MIN(fsn_min_y, y0);
	fsn_max_y = MAX(fsn_max_y, y1);
}


/* ...and its vertical half. Called with pedestal tops and, for files,
 * with box tops (which stand on those pedestals, so they are what
 * actually bounds the scene from above). */
static void
fsn_note_height( double h )
{
	fsn_extent_h = MAX(fsn_extent_h, h);
}


/* Placement pass: top-down. Pins the directory's pedestal at (x, z) on
 * the ground, lays its files out on top of it, and fans its
 * subdirectories out one generation further along the depth axis --
 * centered on this pedestal, each taking as much ground width as its own
 * subtree needs. Sibling subtrees therefore never overlap, at any depth. */
static void
fsn_place( GNode *dnode, double x, double z )
{
	FsnPedestal *ped, *box;
	GNode *node;
	int cols, rows, ndirs = 0, i = 0;
	double grid_w, grid_d;
	double total = 0.0, cursor, span, child_z;

	g_assert( NODE_IS_DIR(dnode) );

	ped = FSN_GEOM_PARAMS(dnode);
	ped->x = x;
	ped->z = z;
	fsn_note_footprint( x - 0.5 * ped->w, x + 0.5 * ped->w,
	    z - 0.5 * ped->d, z + 0.5 * ped->d );
	fsn_note_height( ped->h );

	cols = (int)FSN_DIR_COLS(dnode);
	rows = (int)FSN_DIR_ROWS(dnode);
	grid_w = (cols > 0) ? (cols * FSN_BOX_EDGE + (cols - 1) * FSN_BOX_GAP) : 0.0;
	grid_d = (rows > 0) ? (rows * FSN_BOX_EDGE + (rows - 1) * FSN_BOX_GAP) : 0.0;

	for (node = dnode->children; node != NULL; node = node->next) {
		if (NODE_IS_DIR(node)) {
			++ndirs;
			total += FSN_DIR_SPAN(node);
			continue;
		}

		/* Row-major from the grid's near-left corner. cols is
		 * nonzero whenever this branch is reached at all: it was
		 * derived from the same file count in fsn_measure( ). */
		g_assert( cols > 0 );
		box = FSN_GEOM_PARAMS(node);
		box->x = x - 0.5 * grid_w
		    + (double)(i % cols) * (FSN_BOX_EDGE + FSN_BOX_GAP)
		    + 0.5 * FSN_BOX_EDGE;
		box->z = z - 0.5 * grid_d
		    + (double)(i / cols) * (FSN_BOX_EDGE + FSN_BOX_GAP)
		    + 0.5 * FSN_BOX_EDGE;
		++i;

		fsn_note_height( ped->h + box->h );
	}

	if (ndirs < 1)
		return;

	if (ndirs > 1)
		total += (double)(ndirs - 1) * FSN_SIBLING_GAP;

	cursor = x - 0.5 * total;
	for (node = dnode->children; node != NULL; node = node->next) {
		if (!NODE_IS_DIR(node))
			continue;

		span = FSN_DIR_SPAN(node);
		child_z = z + 0.5 * ped->d + FSN_GENERATION_GAP
		    + 0.5 * FSN_GEOM_PARAMS(node)->d;
		fsn_place( node, cursor + 0.5 * span, child_z );
		cursor += span + FSN_SIBLING_GAP;
	}
}


/* Top-level call to lay out FSN mode. Pure: no gpu.h calls anywhere
 * below this point until the DRAW section. */
void
fsn_geometry_init( GNode *root )
{
	g_assert( root != NULL );
	g_assert( NODE_IS_DIR(root) );

	fsn_min_x = fsn_min_y = G_MAXDOUBLE;
	fsn_max_x = fsn_max_y = -G_MAXDOUBLE;
	fsn_extent_h = 0.0;

	fsn_measure( root );
	fsn_place( root, 0.0, 0.0 );

	fsn_extent_w = fsn_max_x - fsn_min_x;
	fsn_extent_d = fsn_max_y - fsn_min_y;

	fsn_root = root;
	geometry_queue_rebuild( root );
}


/* FSN allocates nothing per node -- the whole layout lives in geometry
 * parameters the node descriptors already carry, and those are freed
 * with the tree itself. So this only drops the module's own state, which
 * is what makes fsn_layout_get( ) start answering NULL again. */
void
fsn_geometry_free( void )
{
	fsn_root = NULL;
	fsn_extent_w = fsn_extent_d = fsn_extent_h = 0.0;
}


const FsnPedestal *
fsn_layout_get( GNode *node )
{
	if (fsn_root == NULL || node == NULL || NODE_IS_METANODE(node))
		return NULL;

	return FSN_GEOM_PARAMS(node);
}


boolean
fsn_layout_wire( GNode *node, FsnWire *wire )
{
	const FsnPedestal *parent, *child;

	if (fsn_root == NULL || node == NULL || node == fsn_root)
		return FALSE;
	if (!NODE_IS_DIR(node))
		return FALSE;
	if (node->parent == NULL || !NODE_IS_DIR(node->parent))
		return FALSE;

	parent = FSN_GEOM_PARAMS(node->parent);
	child = FSN_GEOM_PARAMS(node);

	/* Parent's outward footprint edge -> child's inward one, both at
	 * pedestal-top height. (FsnWire is in world coordinates: the
	 * pedestals' ground `z` is world y, their `h` is world z.) */
	wire->x0 = parent->x;
	wire->y0 = parent->z + 0.5 * parent->d;
	wire->z0 = parent->h;

	wire->x1 = child->x;
	wire->y1 = child->z - 0.5 * child->d;
	wire->z1 = child->h;

	return TRUE;
}


void
fsn_layout_extents( double *width, double *depth, double *height )
{
	if (width != NULL)
		*width = (fsn_root != NULL) ? fsn_extent_w : 0.0;
	if (depth != NULL)
		*depth = (fsn_root != NULL) ? fsn_extent_d : 0.0;
	if (height != NULL)
		*height = (fsn_root != NULL) ? fsn_extent_h : 0.0;
}


/**** DRAW ****************************************************/


/* Task B1, second section. */
void
fsn_geometry_draw( boolean high_detail )
{
	(void)high_detail;
}


/* end geometry-fsn.c */
