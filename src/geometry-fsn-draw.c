/* geometry-fsn-draw.c */

/* FSV_FSN mode, drawing half: turns the layout computed by
 * geometry-fsn.c into gpu.h calls.
 *
 * The split is not cosmetic. The layout half is pure math with no
 * renderer dependency at all, so it belongs to libfsvcore alongside
 * camera.c -- which reads it, to frame the landscape -- and can be
 * exercised headless (tests/test_fsn_layout.c) with no gpu stubs
 * whatsoever. This half calls gpu.h, exactly like geometry.c, and so is
 * frontend-side: both frontends' source lists carry it next to
 * geometry.c, and libfsvcore's other consumers (fsv-scan, the tests)
 * never link it.
 *
 * fsv - 3D File System Visualizer
 * SPDX-License-Identifier:  LGPL-2.1-or-later
 */


#include "common.h"
#include "geometry-fsn.h"

#include "fsn-style.h"
#include "geometry.h"	/* geometry_node_set_color( ) */
#include "gpu.h"
#include "tmaptext.h"


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


/* Cache backing fsn_draw_path_text( ) below. node_absname_display( )
 * walks the node's ancestry, allocates, validates UTF-8 and NFC-composes
 * the result on every call -- its own contract (src/common.c) says it is
 * for callers that fire "on hover/selection changes, not per node per
 * frame", and this one is on the per-frame high-detail path. The path
 * only changes when the current node does, so keep the composed string
 * and recompose only when that pointer moves.
 *
 * Note node_absname_display( ) returns a pointer to ITS static buffer,
 * which the next caller (the status bar, the context menu) will free out
 * from under us -- so this keeps its own copy rather than caching the
 * returned pointer.
 *
 * The key is only ever compared, never dereferenced, and it is a PAIR:
 * the node pointer and the layout generation it was taken in
 * (fsn_layout_generation( ), bumped by every fsn_geometry_init( )).
 * The pointer alone is not enough. fsn_geometry_draw( ) below also
 * drops the cache whenever the layout root changes, but a Change Root
 * or a Rescan frees the whole tree and lays out a new one, and GLib's
 * slice allocator will happily hand the new tree's nodes the addresses
 * the old tree's nodes had -- so a stale key can compare equal to a
 * live, entirely different node, and the ground label would then show
 * the old tree's path. Folding the generation in makes the two keys
 * distinguishable by construction instead of by luck. */
static GNode *path_text_node = NULL;
static unsigned int path_text_generation = 0;
static char *path_text_cache = NULL;

/* Drops the cached path string. Called when the layout goes away, so a
 * freed GNode can never be compared against as a cache key. */
static void
fsn_path_text_invalidate( void )
{
	if (path_text_cache != NULL) {
		xfree( path_text_cache );
		path_text_cache = NULL;
	}
	path_text_node = NULL;
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
	const FsnPedestal *p = fsn_layout_get( fsn_layout_root( ) );
	GNode *node;
	XYZvec pos;
	XYvec dims;

	node = (globals.current_node != NULL) ?
	    globals.current_node : fsn_layout_root( );

	/* fsn_layout_get( ) is documented nullable, and this runs on the
	 * high-detail path where a caller could in principle reach us
	 * between a tree teardown and the next layout */
	if (p == NULL || node == NULL)
		return;

	if (node != path_text_node ||
	    fsn_layout_generation( ) != path_text_generation ||
	    path_text_cache == NULL) {
		fsn_path_text_invalidate( );
		path_text_cache = xstrdup( node_absname_display( node ) );
		path_text_node = node;
		path_text_generation = fsn_layout_generation( );
	}

	pos.x = p->x;
	pos.y = p->z - 0.5 * p->d - FSN_PATH_TEXT_GAP_RATIO * p->w;
	pos.z = FSN_TEXT_LIFT;

	dims.x = FSN_PATH_TEXT_WIDTH_RATIO * p->w;
	dims.y = FSN_PATH_TEXT_HEIGHT_RATIO * p->w;

	text_set_color( FSN_PATH_R, FSN_PATH_G, FSN_PATH_B );
	text_draw_straight( path_text_cache, &pos, &dims );
}


/* Whether `node` is currently on screen. Follows fsn_draw_recursive( )'s
 * own rule exactly, which is less obvious than "some ancestor is
 * collapsed" and easy to get backwards: a directory draws its own box
 * AND its own files unconditionally, at the top of the function, before
 * it ever looks at its own `collapsed`; that flag only gates the loop
 * below it, which recurses into the directory's own CHILD directories.
 * So collapsing a directory D hides D's subdirectories (and, transitively,
 * everything under them) but never D's own files, and never D itself.
 *
 * Consequently:
 *   - a DIRECTORY node's own box draws only once its PARENT's recursion
 *     reaches it, which requires the parent (and the parent's parent,
 *     and so on) to be uncollapsed -- so the walk below starts at
 *     node->parent and checks it, same as every ancestor above it.
 *   - a FILE node is one of its parent's own files, drawn the moment the
 *     parent itself is reached, regardless of the PARENT's own collapsed
 *     flag -- so the walk has to skip that one flag and start checking
 *     one generation further up, at the parent's parent (which is what
 *     actually gates whether the parent was reached at all). A file
 *     directly under the root (root->parent == NULL, so there is no
 *     "one generation further up") is therefore always visible, which
 *     matches reality: root's own recursion always runs. */
static boolean
fsn_node_visible( GNode *node )
{
	GNode *ancestor;

	ancestor = NODE_IS_DIR(node) ? node->parent :
	    (node->parent != NULL ? node->parent->parent : NULL);

	for (; ancestor != NULL; ancestor = ancestor->parent) {
		if (NODE_IS_DIR(ancestor) && DIR_COLLAPSED(ancestor))
			return FALSE;
	}

	return TRUE;
}


/* Draws one ring of the spotlight decal: a filled ellipse, flat at world
 * z, as a single triangle fan (center + a closed loop of perimeter
 * points). Winding follows draw_landscape( )'s ground quad convention
 * (src/sdl/gpu.cpp): angle increasing from 0 is counter-clockwise as
 * seen from +z looking down, which is the visible winding for a decal
 * lying flat on a surface viewed from above -- this app's camera never
 * dips below the landscape (same assumption that quad's own comment
 * documents), so back-face culling losing the decal from underneath is
 * accepted, not a bug. */
static void
fsn_gldraw_spotlight_ring( double cx, double cy, double z,
    double rx, double ry, float alpha )
{
	FsvVertex verts[FSN_SPOTLIGHT_SEGMENTS + 2];
	int i, n = 0;

	verts[n].pos[0] = (float)cx;
	verts[n].pos[1] = (float)cy;
	verts[n].pos[2] = (float)z;
	verts[n].normal[0] = 0.0f;
	verts[n].normal[1] = 0.0f;
	verts[n].normal[2] = 1.0f;
	++n;

	for (i = 0; i <= FSN_SPOTLIGHT_SEGMENTS; i++) {
		double theta = 2.0 * G_PI * (double)i / (double)FSN_SPOTLIGHT_SEGMENTS;

		verts[n].pos[0] = (float)(cx + rx * cos( theta ));
		verts[n].pos[1] = (float)(cy + ry * sin( theta ));
		verts[n].pos[2] = (float)z;
		verts[n].normal[0] = 0.0f;
		verts[n].normal[1] = 0.0f;
		verts[n].normal[2] = 1.0f;
		++n;
	}

	gpu_set_color( 1.0f, 1.0f, 1.0f, alpha );
	gpu_draw( FSV_TRIANGLE_FAN, verts, n, NULL, 0 );
}


/* One truncated translucent cone: apex ellipse (APEX_FRAC of the base)
 * at apex_z, base ellipse (the spotlight pool's own rx/ry) at base_z,
 * as a single triangle strip around the perimeter. No caps: the pool
 * rings already paint the base, and the apex is open sky. Winding puts
 * the outward faces front (the pipeline back-face culls, gpu.cpp), so
 * the viewer sees exactly one translucent layer -- the near side of the
 * beam. */
static void
fsn_gldraw_spotlight_cone( double cx, double cy, double base_z,
    double apex_z, double rx, double ry )
{
	FsvVertex verts[2 * (FSN_SPOTLIGHT_SEGMENTS + 1)];
	int i, n = 0;

	for (i = 0; i <= FSN_SPOTLIGHT_SEGMENTS; i++) {
		double theta = 2.0 * G_PI * (double)i / (double)FSN_SPOTLIGHT_SEGMENTS;
		double c = cos( theta ), s = sin( theta );

		verts[n].pos[0] = (float)(cx + FSN_SPOTLIGHT_CONE_APEX_FRAC * rx * c);
		verts[n].pos[1] = (float)(cy + FSN_SPOTLIGHT_CONE_APEX_FRAC * ry * s);
		verts[n].pos[2] = (float)apex_z;
		verts[n].normal[0] = (float)c;
		verts[n].normal[1] = (float)s;
		verts[n].normal[2] = 0.0f;
		++n;
		verts[n].pos[0] = (float)(cx + rx * c);
		verts[n].pos[1] = (float)(cy + ry * s);
		verts[n].pos[2] = (float)base_z;
		verts[n].normal[0] = (float)c;
		verts[n].normal[1] = (float)s;
		verts[n].normal[2] = 0.0f;
		++n;
	}

	gpu_set_color( 1.0f, 1.0f, 1.0f, FSN_SPOTLIGHT_CONE_ALPHA );
	gpu_draw( FSV_TRIANGLE_STRIP, verts, n, NULL, 0 );
}


/* Selection spotlight (Task B3, US5861885's literal ground-glow under
 * the selected node): a soft white elliptical light pool on the surface
 * the current node actually stands on -- world z == 0 (the true ground)
 * for a directory, or its parent pedestal's top (world z == parent->h)
 * for a file, since that pedestal top is "the ground" a file box sits
 * on. See FSN_SPOTLIGHT_LIFT's comment (fsn-style.h) for why that lift
 * is its own constant rather than reusing FSN_TEXT_LIFT.
 *
 * SELECT PASS: this decal is not a node, has no id, and -- unlike
 * fsn_gldraw_wire( )'s black-not-skipped wires -- must not be drawn at
 * all: it is alpha-blended, so painting it into the pick target with any
 * color, id or otherwise, would blend partial coverage into whatever id
 * color is already there and corrupt the read-back. Checked here
 * (gpu_render_mode( )) rather than by the caller, matching
 * fsn_gldraw_wire( )'s own self-check a few functions up.
 *
 * OVERVIEW PASS (Task C1): skipped there too, for a different reason --
 * legibility, not correctness. The glow is FSN_SPOTLIGHT_DIR_SCALE times
 * the selected pedestal's own footprint, which at the mini-map's scale
 * covers a good part of the visible landscape as a shapeless pale smear
 * over the very pedestals it is meant to point at. The overview marks
 * the *camera*, not the selection (see src/sdl/ui_overview.cpp). Checked
 * here for the same reason as the select-pass test above: the decision
 * belongs to the draw that knows what it is drawing. */
static void
fsn_draw_spotlight( void )
{
	GNode *node = globals.current_node;
	const FsnPedestal *ped, *parent_ped;
	double cx, cz, base_z, rx, rz;
	double apex_z;
	int i;

	if (gpu_render_mode( ) != FSV_RENDER_NORMAL)
		return;

	if (gpu_overview_pass( ))
		return;

	if (globals.fsv_mode != FSV_FSN || node == NULL)
		return;

	ped = fsn_layout_get( node );
	if (ped == NULL)
		return; /* no layout pass yet, or node is the metanode */

	if (!fsn_node_visible( node ))
		return; /* a collapsed ancestor hides it */

	if (NODE_IS_DIR(node)) {
		cx = ped->x;
		cz = ped->z;
		base_z = 0.0; /* directories stand on the true ground */
		rx = FSN_SPOTLIGHT_DIR_SCALE * 0.5 * ped->w;
		rz = FSN_SPOTLIGHT_DIR_SCALE * 0.5 * ped->d;
	}
	else {
		if (node->parent == NULL || !NODE_IS_DIR(node->parent))
			return; /* shouldn't happen -- every file has a dir parent */
		parent_ped = fsn_layout_get( node->parent );
		if (parent_ped == NULL)
			return;
		cx = ped->x;
		cz = ped->z;
		base_z = parent_ped->h; /* files stand on their parent's pedestal top */
		rx = FSN_SPOTLIGHT_FILE_SCALE * 0.5 * ped->w;
		rz = FSN_SPOTLIGHT_FILE_SCALE * 0.5 * ped->d;
	}

	gpu_set_lighting( 0 );
	gpu_set_depth_test( FSV_DEPTH_LESS_NOWRITE );

	for (i = 0; i < FSN_SPOTLIGHT_RING_COUNT; i++)
		fsn_gldraw_spotlight_ring( cx, cz, base_z + FSN_SPOTLIGHT_LIFT,
		    fsn_spotlight_rings[i].radius_frac * rx,
		    fsn_spotlight_rings[i].radius_frac * rz,
		    fsn_spotlight_rings[i].alpha );

	/* The beam above the pool -- see FSN_SPOTLIGHT_CONE_ALPHA's comment
	 * (fsn-style.h) for why the pool alone was not enough. Height rides
	 * the node's own height with a floor, so a tall pedestal's beam
	 * still clears it and a flat file box's beam is not a needle. */
	apex_z = base_z + MAX(FSN_SPOTLIGHT_CONE_MIN_HEIGHT,
	    FSN_SPOTLIGHT_CONE_HEIGHT_MULT * ped->h);
	fsn_gldraw_spotlight_cone( cx, cz, base_z + FSN_SPOTLIGHT_LIFT,
	    apex_z, rx, rz );

	gpu_set_depth_test( FSV_DEPTH_LESS );
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
	static GNode *drawn_root = NULL;
	GNode *root = fsn_layout_root( );

	/* A new (or absent) layout means a new tree: drop the cached path
	 * string, so its GNode cache key can never outlive the nodes it was
	 * taken from. Deliberately here and not in fsn_geometry_free( ) --
	 * that lives in the layout half, which libfsvcore links and this
	 * file is absent from, so it cannot call into here without breaking
	 * the split.
	 *
	 * This is the cheap check, not the load-bearing one: `root` is
	 * itself a GNode * and a rescan of the same directory can (and
	 * routinely does) get the same address back, so this comparison
	 * alone would miss a genuine tree change. The cache key carries the
	 * layout generation for exactly that reason -- see
	 * fsn_draw_path_text( )'s note above. Both are kept: this one drops
	 * the allocation promptly when the mode changes, the generation
	 * makes correctness independent of the allocator. */
	if (root != drawn_root) {
		fsn_path_text_invalidate( );
		drawn_root = root;
	}

	if (root == NULL)
		return;

	fsn_draw_recursive( root, FSN_DRAW_GEOMETRY );

	/* After the solid geometry, so its FSV_DEPTH_LESS_NOWRITE test sees
	 * every pedestal/box's real depth already written -- see
	 * fsn_draw_spotlight( )'s own SELECT PASS note for why this call runs
	 * unconditionally (including from gpu_pick( )'s high_detail == FALSE
	 * pass) rather than being gated here alongside the high_detail return
	 * below: the self-check inside is what actually keeps it out of the
	 * select pass. */
	fsn_draw_spotlight( );

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
	fsn_draw_recursive( root, FSN_DRAW_LABELS );
	fsn_draw_path_text( );
	text_post( );
}


/* end geometry-fsn-draw.c */
