/* geometry-fsn.h */

/* FSV_FSN mode: the original fsn's pedestal-and-wire landscape.
 *
 * Two implementation files, split along the renderer boundary:
 *   src/geometry-fsn.c       layout -- pure math, part of libfsvcore
 *   src/geometry-fsn-draw.c  drawing -- gpu.h, part of each frontend
 * (geometry.c's own per-mode families would have been the other home
 * for all this, but that file is already ~2.9k lines.)
 *
 * fsv - 3D File System Visualizer
 * SPDX-License-Identifier:  LGPL-2.1-or-later
 */


#ifdef FSV_GEOMETRY_FSN_H
	#error
#endif
#define FSV_GEOMETRY_FSN_H


/* Geometry parameters for a node in FSN mode.
 *
 * COORDINATES. Field names follow task-B1-brief.md's interface, which
 * uses the graphics convention (x/z on the ground, y up). fsv's world
 * does NOT: it is z-up, exactly as MapV and TreeV use it (x/y are the
 * ground plane, z is height above it -- see geometry_mapv_node_z0( )).
 * So, once and for all:
 *
 *     FsnPedestal::x  ->  world x   (ground, left/right)
 *     FsnPedestal::z  ->  world y   (ground, depth: the direction the
 *                                    tree grows, away from the initial
 *                                    camera)
 *     FsnPedestal::h  ->  world z   (height above the ground plane)
 *
 * Everything in this struct is ground-plane geometry; only the draw pass
 * and FsnWire below speak world coordinates.
 *
 * Used for both kinds of object FSN draws, which is why it is five plain
 * numbers and not two structs:
 *   - a directory: its pedestal (a wide low slab standing on the ground,
 *     base at world z == 0, top at z == h);
 *   - a file: its box on the parent directory's pedestal top (base at
 *     the parent's h, top at the parent's h + this h).
 *
 * Stored in the node descriptor's own scratch space, like every other
 * mode's params (NodeDesc::geomparams is exactly 5 doubles, so this
 * struct fills it precisely). */
typedef struct _FsnPedestal FsnPedestal;
struct _FsnPedestal {
	double	x;	/* center, ground left/right */
	double	z;	/* center, ground depth (-> world y) */
	double	w;	/* full width  (x extent) */
	double	d;	/* full depth  (z extent) */
	double	h;	/* height (bottom to top) */
};

/* One parent->child wire, in WORLD coordinates (z is up here, unlike
 * FsnPedestal above). Endpoint 0 is on the parent's outward footprint
 * edge at the parent's pedestal top; endpoint 1 is on the child's inward
 * footprint edge at the child's pedestal top. */
typedef struct _FsnWire FsnWire;
struct _FsnWire {
	double	x0, y0, z0;
	double	x1, y1, z1;
};

#define FSN_GEOM_PARAMS(node)	((FsnPedestal *)(NODE_DESC(node)->geomparams))


/* Layout pass. PURE: computes and stores geometry for the whole tree
 * rooted at `root` (which must be a directory node -- root_dnode) and
 * makes no gpu.h call whatsoever, so it can be exercised headless. See
 * tests/test_fsn_layout.c. */
void fsn_geometry_init( GNode *root );

/* Draw pass. Everything that touches gpu.h lives behind this. */
void fsn_geometry_draw( boolean high_detail );

/* Drops the layout (see the note on the definition -- FSN allocates
 * nothing per node, so this only clears module state). */
void fsn_geometry_free( void );

/* Layout accessors -- the testable surface, and what camera.c reads to
 * frame the scene. fsn_layout_get( ) returns NULL if `node` has no FSN
 * geometry (no layout pass has run yet, or it is the metanode);
 * fsn_layout_root( ) returns the directory the current layout was built
 * from, or NULL when there is none. */
GNode *fsn_layout_root( void );
const FsnPedestal *fsn_layout_get( GNode *node );

/* Fills in the wire running from `node`'s parent directory to `node`.
 * Returns FALSE (leaving *wire untouched) for a node that has no such
 * wire: the root directory, the metanode, and any non-directory. */
boolean fsn_layout_wire( GNode *node, FsnWire *wire );

/* Bounding box of the whole laid-out landscape: ground width (x extent),
 * ground depth (world y extent) and peak height. Any argument may be
 * NULL. All three come back 0.0 if no layout pass has run. */
void fsn_layout_extents( double *width, double *depth, double *height );


/* end geometry-fsn.h */
