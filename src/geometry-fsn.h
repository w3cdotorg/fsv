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
 * mode's params (NodeDesc::geomparams is 6 doubles -- MapV's 2026
 * squarify rework grew it from 5 to fit area_weight -- so this 5-double
 * struct fills the first 5 of those 6, with one spare left over). */
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


/* Layout pass: computes and stores geometry for the whole tree rooted
 * at `root` (which must be a directory node -- root_dnode).
 *
 * Makes no gpu.h call whatsoever, which is what lets it be exercised
 * headless -- and is enforced by the linker, not by convention: this
 * file's implementation is part of libfsvcore, and
 * tests/test_fsn_layout.c links it with no renderer stubs at all.
 *
 * Renderer-free is not side-effect-free, though. Like MapV's
 * mapv_init_recursive( ), it reads the frontend's expand/collapse state
 * (dirtree_entry_expanded( )) and writes each directory's `deployment`
 * to match, so the draw pass and colexp.c start out agreeing with the
 * directory tree. */
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

/* Incremented by every fsn_geometry_init( ) call, i.e. once per layout
 * pass. Exists so that a consumer caching something keyed on a GNode *
 * can tell "the same node" from "a different node the allocator happened
 * to hand back at the same address" -- GLib's slice allocator reuses
 * freed node addresses aggressively, and after a Rescan or a Change Root
 * a stale key can compare equal to a live pointer that means something
 * else entirely. Comparing generations as well makes that impossible
 * rather than merely unlikely. Wraps after 2^32 layout passes, which
 * would require one rescan per second for 136 years. */
unsigned int fsn_layout_generation( void );

/* Fills in the wire running from `node`'s parent directory to `node`.
 * Returns FALSE (leaving *wire untouched) for a node that has no such
 * wire: the root directory, the metanode, and any non-directory. */
boolean fsn_layout_wire( GNode *node, FsnWire *wire );

/* Bounding box of the whole laid-out landscape: ground width (x extent),
 * ground depth (world y extent) and peak height. Any argument may be
 * NULL. All three come back 0.0 if no layout pass has run. */
void fsn_layout_extents( double *width, double *depth, double *height );

/* The same ground-plane bounding box as fsn_layout_extents( ), but as
 * absolute WORLD coordinates rather than sizes: the landscape is not
 * centered on the origin (fsn_place( ) pins the root pedestal at (0,0)
 * and grows the tree towards +y), so a consumer that has to *frame* the
 * landscape -- fsn-mode Task C1's overview mini-map, which builds a
 * top-down orthographic projection around it -- needs the corners, not
 * the extents. `min_y`/`max_y` are world y, i.e. the FsnPedestal::z axis
 * (see the COORDINATES note at the top of this header).
 *
 * Any argument may be NULL. All four come back 0.0 if no layout pass has
 * run, which is a degenerate (empty) box the caller must handle -- same
 * convention as fsn_layout_extents( ). */
void fsn_layout_bounds( double *min_x, double *max_x, double *min_y,
			double *max_y );

/* The directory whose pedestal center is nearest the ground point
 * (`x`, `y`) in WORLD coordinates, or NULL if no layout pass has run.
 *
 * Only directories are candidates: they are what a pedestal is, and what
 * camera_look_at( ) is worth flying to. Only *drawn* ones, too -- the
 * walk descends exactly like the draw pass's own recursion, so a
 * collapsed directory's hidden children can never be returned for a
 * click on a spot where nothing is visible.
 *
 * Pure distance to the pedestal center, not a footprint hit test: this
 * answers "which pedestal did the user mean", including for a click on
 * bare ground between two of them, which is what fsn-mode Task C1's
 * click-to-look-at wants. */
GNode *fsn_layout_nearest( double x, double y );


/* end geometry-fsn.h */
