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
#include "geometry.h"	/* geometry_queue_rebuild( ), geometry_node_set_color( ) */
#include "gpu.h"
#include "tmaptext.h"


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


/* Messages for fsn_draw_recursive( ), same idiom as MapV's
 * mapv_draw_recursive( ): one tree walk, two payloads, because labels
 * have to be drawn in a single text_pre( )/text_post( ) bracket after
 * all the solid geometry. */
enum {
	FSN_DRAW_GEOMETRY,
	FSN_DRAW_LABELS
};


/* Draws one axis-aligned box: four side faces and a top face, no bottom
 * (nothing ever sees one -- pedestals stand on the ground and file boxes
 * on a pedestal top). Vertex/index layout and, importantly, winding are
 * MapV's mapv_gldraw_node( ) minus the slanted sides: the renderer culls
 * back faces with front == counter-clockwise (src/sdl/gpu.cpp's
 * rasterizer_state, ogl.c's glEnable(GL_CULL_FACE)), so getting the
 * order wrong would silently drop half of every box.
 *
 * SELECT PASS: color and lighting come from geometry_node_set_color( ),
 * which paints the node's flat id color when the renderer is resolving a
 * pick. Pedestals and file boxes are therefore both pickable, which is
 * the whole point -- they are the only FSN geometry that *is* a node. */
static void
fsn_gldraw_box( const FsnPedestal *p, double base_z, GNode *node )
{
	const double x0 = p->x - 0.5 * p->w, x1 = p->x + 0.5 * p->w;
	const double y0 = p->z - 0.5 * p->d, y1 = p->z + 0.5 * p->d;
	const double zb = base_z, zt = base_z + p->h;

	const FsvVertex vert[] = {
		/* Rear face (+y) */
		{{x0, y1, zb}, {0.0f, 1.0f, 0.0f}},
		{{x0, y1, zt}, {0.0f, 1.0f, 0.0f}},
		{{x1, y1, zb}, {0.0f, 1.0f, 0.0f}},
		{{x1, y1, zt}, {0.0f, 1.0f, 0.0f}},
		/* Right face (+x) */
		{{x1, y1, zb}, {1.0f, 0.0f, 0.0f}},
		{{x1, y1, zt}, {1.0f, 0.0f, 0.0f}},
		{{x1, y0, zb}, {1.0f, 0.0f, 0.0f}},
		{{x1, y0, zt}, {1.0f, 0.0f, 0.0f}},
		/* Front face (-y) */
		{{x1, y0, zb}, {0.0f, -1.0f, 0.0f}},
		{{x1, y0, zt}, {0.0f, -1.0f, 0.0f}},
		{{x0, y0, zb}, {0.0f, -1.0f, 0.0f}},
		{{x0, y0, zt}, {0.0f, -1.0f, 0.0f}},
		/* Left face (-x) */
		{{x0, y0, zb}, {-1.0f, 0.0f, 0.0f}},
		{{x0, y0, zt}, {-1.0f, 0.0f, 0.0f}},
		{{x0, y1, zb}, {-1.0f, 0.0f, 0.0f}},
		{{x0, y1, zt}, {-1.0f, 0.0f, 0.0f}},
		/* Top face (+z) */
		{{x0, y0, zt}, {0.0f, 0.0f, 1.0f}},
		{{x1, y0, zt}, {0.0f, 0.0f, 1.0f}},
		{{x0, y1, zt}, {0.0f, 0.0f, 1.0f}},
		{{x1, y1, zt}, {0.0f, 0.0f, 1.0f}}
	};
	static const unsigned int elements[] = {
		0,  1,  2,  2,  1,  3,	/* Rear */
		4,  5,  6,  6,  5,  7,	/* Right */
		8,  9,  10, 10, 9,  11,	/* Front */
		12, 13, 14, 14, 13, 15,	/* Left */
		16, 17, 18, 18, 17, 19	/* Top */
	};

	geometry_node_set_color( node );
	gpu_draw( FSV_TRIANGLES, vert, G_N_ELEMENTS(vert),
	    elements, G_N_ELEMENTS(elements) );
}


/* Draws the wire from a directory's pedestal to one child's.
 * `deployment` is the parent's, and scales the child end's height so the
 * wire tracks the child subtree as it grows in or shrinks away.
 *
 * SELECT PASS: a wire is not a node, so it has no id to paint -- it is
 * drawn BLACK (== id 0, "nothing there") rather than skipped, exactly
 * like geometry.c's TreeV branch connectors: skipping would drop it from
 * the pick pass's depth buffer, and a wire that occludes something on
 * screen must occlude it in the pick pass too. */
static void
fsn_gldraw_wire( const FsnWire *w, double deployment )
{
	const FsvVertex vert[] = {
		{{w->x0, w->y0, w->z0}, {0.0f, 0.0f, 0.0f}},
		{{w->x1, w->y1, w->z1 * deployment}, {0.0f, 0.0f, 0.0f}}
	};

	if (gpu_render_mode( ) == FSV_RENDER_NORMAL)
		gpu_set_color( FSN_WIRE_R, FSN_WIRE_G, FSN_WIRE_B, 1.0f );
	else
		gpu_set_color( 0.0f, 0.0f, 0.0f, 1.0f );
	gpu_set_lighting( 0 );
	gpu_set_line_width( FSN_WIRE_WIDTH );

	gpu_draw( FSV_LINES, vert, G_N_ELEMENTS(vert), NULL, 0 );
}


/* Draws a directory's name label, flat on the clear margin band along
 * the near edge of its pedestal top (the middle of that face is covered
 * in file boxes). Lifted FSN_TEXT_LIFT off the surface so it cannot
 * z-fight with it. */
static void
fsn_apply_label( GNode *dnode )
{
	const FsnPedestal *p = FSN_GEOM_PARAMS(dnode);
	XYZvec pos;
	XYvec dims;

	pos.x = p->x;
	pos.y = p->z - 0.5 * p->d + 0.5 * FSN_PEDESTAL_MARGIN;
	pos.z = p->h + FSN_TEXT_LIFT;

	dims.x = 0.9 * p->w;
	dims.y = 0.75 * FSN_PEDESTAL_MARGIN;

	text_draw_straight( NODE_DNAME(dnode), &pos, &dims );
}


/* The current node's absolute path, written large on the ground in front
 * of the root pedestal -- the reference screenshot's "/usr/people/kip".
 * Non-pickable by construction: it is text, and gpu_pick( ) calls
 * geometry_draw(FALSE), so no label of any kind is drawn in the select
 * pass at all (fsn_geometry_draw( ) gates the whole text block on
 * high_detail). */
static void
fsn_draw_path_text( void )
{
	const FsnPedestal *p = FSN_GEOM_PARAMS(fsn_root);
	GNode *node;
	XYZvec pos;
	XYvec dims;

	node = (globals.current_node != NULL) ? globals.current_node : fsn_root;

	pos.x = p->x;
	pos.y = p->z - 0.5 * p->d - FSN_PATH_TEXT_GAP;
	pos.z = FSN_TEXT_LIFT;

	dims.x = MAX(fsn_extent_w, 4.0 * p->w);
	dims.y = FSN_PATH_TEXT_HEIGHT;

	text_set_color( FSN_PATH_R, FSN_PATH_G, FSN_PATH_B );
	text_draw_straight( node_absname_display( node ), &pos, &dims );
}


/* FSN mode "full draw", one directory and everything under it.
 *
 * Deployment drives the recursion exactly as it does in MapV: a
 * collapsed directory shows its pedestal and its own files but neither
 * its children nor their wires, and a directory caught mid-morph draws
 * its children under a z-only scale, so a subtree grows up out of the
 * ground plane rather than popping in. Positions never move -- the
 * layout is deployment-independent -- so only heights animate. */
static void
fsn_draw_recursive( GNode *dnode, int action )
{
	DirNodeDesc *dir_ndesc;
	const FsnPedestal *ped;
	GNode *node;
	FsnWire wire;
	mat4 tmpmat;
	double deployment;
	boolean collapsed, scaled;

	g_assert( NODE_IS_DIR(dnode) );

	dir_ndesc = DIR_NODE_DESC(dnode);
	ped = FSN_GEOM_PARAMS(dnode);
	deployment = dir_ndesc->deployment;
	collapsed = DIR_COLLAPSED(dnode);

	if (action == FSN_DRAW_GEOMETRY) {
		fsn_gldraw_box( ped, 0.0, dnode );

		/* Files, standing on this pedestal's top face */
		for (node = dnode->children; node != NULL; node = node->next)
			if (!NODE_IS_DIR(node))
				fsn_gldraw_box( FSN_GEOM_PARAMS(node),
				    ped->h, node );
	}
	else
		fsn_apply_label( dnode );

	/* Keep geometry.c's colexp bookkeeping honest (see
	 * geometry_colexp_in_progress( )) */
	dir_ndesc->geom_expanded = !collapsed;

	if (collapsed)
		return;

	scaled = !DIR_EXPANDED(dnode);

	for (node = dnode->children; node != NULL; node = node->next) {
		if (!NODE_IS_DIR(node))
			continue;

		/* The wire is drawn in the *parent's* frame, unscaled, with
		 * the child end's height scaled by hand -- putting it inside
		 * the scale below would drag the parent end down with it */
		if ((action == FSN_DRAW_GEOMETRY) &&
		    fsn_layout_wire( node, &wire ))
			fsn_gldraw_wire( &wire, deployment );

		if (scaled) {
			glm_mat4_copy( gpu_mat.modelview, tmpmat );
			glm_scale( gpu_mat.modelview,
			    (vec3){ 1.0f, 1.0f, (float)deployment } );
			gpu_upload_matrices( );
		}

		fsn_draw_recursive( node, action );

		if (scaled) {
			glm_mat4_copy( tmpmat, gpu_mat.modelview );
			gpu_upload_matrices( );
		}
	}
}


/* Draws FSN geometry */
void
fsn_geometry_draw( boolean high_detail )
{
	if (fsn_root == NULL)
		return;

	fsn_draw_recursive( fsn_root, FSN_DRAW_GEOMETRY );

	/* Line width is per-draw-state, not per-batch, and the wires just
	 * set it: hand the default back the way cursor_post( ) does */
	gpu_set_line_width( 1.0f );

	if (!high_detail)
		return;

	/* High-detail extras. gpu_pick( ) draws with high_detail == FALSE,
	 * so everything below is absent from the select pass by
	 * construction -- see fsn_draw_path_text( ). */
	text_pre( );
	text_set_color( 0.0f, 0.0f, 0.0f ); /* name labels are black, as in MapV */
	fsn_draw_recursive( fsn_root, FSN_DRAW_LABELS );
	fsn_draw_path_text( );
	text_post( );
}


/* end geometry-fsn.c */
