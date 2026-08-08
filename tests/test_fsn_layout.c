/* tests/test_fsn_layout.c
 *
 * SPDX-License-Identifier: MIT
 *
 * Headless invariants of FSV_FSN mode's layout pass (Task B1 of the
 * fsn-mode plan). Scans tests/fixture, runs fsn_geometry_init( ) -- which
 * is required to be pure math, no gpu.h calls -- and checks the structural
 * properties the pedestal/wire landscape is defined by:
 *
 *   1. the root directory's pedestal sits at the ground origin;
 *   2. every child directory's pedestal sits strictly in front of (further
 *      along the ground depth axis than) its parent's, clear of it;
 *   3. pedestal height rises with subtree size (dir-a holds dir-b, so
 *      dir-a must be taller);
 *   4. every child's wire runs from a point ON its parent's outward
 *      footprint edge to a point ON its own inward one -- checked over
 *      the whole tree, not just one node;
 *   5. sibling *subtrees* occupy disjoint bands of ground width, at every
 *      depth. This is the load-bearing half of the layout (the
 *      FSN_DIR_SPAN measuring pass and the FSN_SIBLING_GAP slicing in the
 *      placement pass) and the reason tests/fixture carries two sibling
 *      directories, dir-a and dir-c, rather than one;
 *   6. file boxes stand inside their directory's pedestal top face.
 *
 * LINKING. src/geometry-fsn.c is gpu-free by construction and therefore
 * part of libfsvcore, so this links exactly like test_scanfs -- core
 * objects plus tools/fsv-headless-stubs.c, and not one renderer stub.
 * That is the invariant, not an accident: the day the layout pass grows
 * a gpu.h call, this test stops linking.
 */

#include <assert.h>
#include <math.h>
#include <string.h>

#include "common.h" /* pulls in glib.h, and must precede it: G_LOG_DOMAIN */
#include "fsv.h"
#include "geometry-fsn.h"
#include "scanfs.h"

/* Generous: every quantity checked here is O(100) world units, and the
 * layout is plain double arithmetic with no accumulation to speak of. */
#define TOL 1.0e-9

static GNode *
child_named(GNode *parent, const char *name)
{
	GNode *node;

	for (node = parent->children; node != NULL; node = node->next)
		if (strcmp(NODE_DESC(node)->name, name) == 0)
			return node;

	return NULL;
}


/* Ground-width band the whole subtree under `dnode` occupies: the union
 * of every pedestal footprint in it. Recomputed here from the public
 * accessor rather than read out of FSN_DIR_SPAN, so the test checks the
 * placement that actually resulted and not the intermediate the layout
 * happened to write down. */
static void
subtree_x_band(GNode *dnode, double *lo, double *hi)
{
	const FsnPedestal *p = fsn_layout_get(dnode);
	GNode *node;

	assert(p != NULL);
	*lo = p->x - 0.5 * p->w;
	*hi = p->x + 0.5 * p->w;

	for (node = dnode->children; node != NULL; node = node->next) {
		double clo, chi;

		if (!NODE_IS_DIR(node))
			continue;
		subtree_x_band(node, &clo, &chi);
		*lo = MIN(*lo, clo);
		*hi = MAX(*hi, chi);
	}
}


/* Is (x, y) a point on the given footprint's outward (+depth) or inward
 * (-depth) edge segment? "On the edge" means exactly on that edge's
 * line, and within the footprint's width -- which is what "the wire
 * touches the pedestal" has to mean for the wire to visually land on it.
 * A wire anchored at a pedestal's center, at its far edge, or off past
 * its corner all fail this. */
static int
on_footprint_edge(const FsnPedestal *p, double x, double y, int outward)
{
	const double edge = outward ? (p->z + 0.5 * p->d) : (p->z - 0.5 * p->d);

	if (fabs(y - edge) > TOL)
		return 0;

	return (x >= p->x - 0.5 * p->w - TOL) && (x <= p->x + 0.5 * p->w + TOL);
}


/* Walks the whole tree asserting invariants 2, 4 and 5 at every level.
 * Returns the number of directories visited, so the caller can prove the
 * walk actually reached the fixture's three subdirectories rather than
 * vacuously passing on an empty loop. */
static int
check_subtree(GNode *dnode)
{
	const FsnPedestal *parent = fsn_layout_get(dnode);
	GNode *a, *b;
	int visited = 1;

	assert(parent != NULL);

	for (a = dnode->children; a != NULL; a = a->next) {
		const FsnPedestal *child;
		FsnWire wire;
		double alo, ahi, blo, bhi;

		if (!NODE_IS_DIR(a)) {
			/* 6. file box stands on this pedestal's top face */
			const FsnPedestal *box = fsn_layout_get(a);
			assert(box != NULL);
			assert(box->w > 0.0 && box->d > 0.0 && box->h > 0.0);
			assert((box->x - 0.5 * box->w) > (parent->x - 0.5 * parent->w));
			assert((box->x + 0.5 * box->w) < (parent->x + 0.5 * parent->w));
			assert((box->z - 0.5 * box->d) > (parent->z - 0.5 * parent->d));
			assert((box->z + 0.5 * box->d) < (parent->z + 0.5 * parent->d));
			continue;
		}

		child = fsn_layout_get(a);
		assert(child != NULL);

		/* 2. in front of the parent, clear of its footprint */
		assert(child->z > parent->z);
		assert((child->z - 0.5 * child->d) > (parent->z + 0.5 * parent->d));

		/* 4. wire endpoints land on both footprints' facing edges */
		assert(fsn_layout_wire(a, &wire));
		assert(on_footprint_edge(parent, wire.x0, wire.y0, 1));
		assert(on_footprint_edge(child, wire.x1, wire.y1, 0));
		assert(fabs(wire.z0 - parent->h) < TOL);
		assert(fabs(wire.z1 - child->h) < TOL);
		/* ...and it actually spans the gap between them */
		assert(wire.y1 > wire.y0);

		/* 5. this subtree's ground band is disjoint from each later
		 * sibling subtree's -- the span slicing, checked on the
		 * placement rather than on the span values that produced it */
		subtree_x_band(a, &alo, &ahi);
		for (b = a->next; b != NULL; b = b->next) {
			if (!NODE_IS_DIR(b))
				continue;
			subtree_x_band(b, &blo, &bhi);
			assert(ahi < blo || bhi < alo);
		}

		visited += check_subtree(a);
	}

	return visited;
}

int
main(void)
{
	GNode *root, *dir_a, *dir_b, *dir_c, *file1;
	const FsnPedestal *p_root, *p_a, *p_b, *p_c;
	FsnWire wire;
	double width, depth, height;
	double band_a_lo, band_a_hi, band_c_lo, band_c_hi;
	int dirs_visited;

	scanfs(FIXTURE_DIR); /* FIXTURE_DIR injected by meson (-D) */
	assert(globals.fstree != NULL);

	root = root_dnode;
	assert(root != NULL);
	dir_a = child_named(root, "dir-a");
	assert(dir_a != NULL && NODE_IS_DIR(dir_a));
	dir_b = child_named(dir_a, "dir-b");
	assert(dir_b != NULL && NODE_IS_DIR(dir_b));
	dir_c = child_named(root, "dir-c");
	assert(dir_c != NULL && NODE_IS_DIR(dir_c));
	file1 = child_named(root, "file1.txt");
	assert(file1 != NULL && !NODE_IS_DIR(file1));

	/* No layout yet */
	assert(fsn_layout_get(root) == NULL);

	fsn_geometry_init(root);

	p_root = fsn_layout_get(root);
	p_a = fsn_layout_get(dir_a);
	p_b = fsn_layout_get(dir_b);
	p_c = fsn_layout_get(dir_c);
	assert(p_root != NULL && p_a != NULL && p_b != NULL && p_c != NULL);

	/* 1. Root pedestal at the ground origin, with a real footprint */
	assert(fabs(p_root->x) < TOL);
	assert(fabs(p_root->z) < TOL);
	assert(p_root->w > 0.0 && p_root->d > 0.0 && p_root->h > 0.0);

	/* 3. Pedestal height monotonic in subtree size. dir-a's subtree
	 * strictly contains dir-b's (dir-b itself, plus file2.bin), so
	 * dir-a's pedestal must be the taller of the two. */
	assert(DIR_NODE_DESC(dir_a)->subtree.size >
	       DIR_NODE_DESC(dir_b)->subtree.size);
	assert(p_a->h > p_b->h);
	assert(p_root->h >= p_a->h);

	/* Nothing above the root, and nothing at all for a file: neither has
	 * a parent directory to be wired to */
	assert(!fsn_layout_wire(root, &wire));
	assert(!fsn_layout_wire(file1, &wire));

	/* 2, 4, 5, 6 over the whole tree. The fixture has exactly five
	 * subdirectories under the root (dir-a, its dir-b/dir-d/dir-e, and
	 * dir-c), so a count of six visited directories is what proves the
	 * walk really descended rather than passing on an empty loop. */
	dirs_visited = check_subtree(root);
	assert(dirs_visited == 6);

	/* 5, spelled out for the case the fixture's shape exists to create.
	 * dir-a holds THREE subdirectories, so its subtree is several times
	 * wider than its own pedestal -- which is the only situation in
	 * which FSN_DIR_SPAN's "max(own width, children's total)" rule has
	 * any effect at all. Were the measuring pass to allot dir-a only its
	 * own pedestal width, its three children would spread far past that
	 * slice and collide with dir-c's band. Both halves of the slicing
	 * were confirmed to fail this test when broken on purpose: the
	 * measuring rule here, and the placement cursor's FSN_SIBLING_GAP
	 * step inside check_subtree( ). */
	subtree_x_band(dir_a, &band_a_lo, &band_a_hi);
	subtree_x_band(dir_c, &band_c_lo, &band_c_hi);
	assert(band_a_hi < band_c_lo || band_c_hi < band_a_lo);
	/* Both bands sit inside the root's own subtree band */
	{
		double band_root_lo, band_root_hi;

		subtree_x_band(root, &band_root_lo, &band_root_hi);
		assert(band_root_lo <= MIN(band_a_lo, band_c_lo) + TOL);
		assert(band_root_hi >= MAX(band_a_hi, band_c_hi) - TOL);
	}

	/* Extents cover at least the root and the deepest child */
	fsn_layout_extents(&width, &depth, &height);
	assert(width >= p_root->w);
	assert(depth >= (p_b->z + 0.5 * p_b->d) - (p_root->z - 0.5 * p_root->d));
	assert(height >= p_root->h);

	fsn_geometry_free();
	assert(fsn_layout_get(root) == NULL);

	return 0;
}
