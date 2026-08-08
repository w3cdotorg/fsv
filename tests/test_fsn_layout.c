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
 *   2. a child directory's pedestal sits strictly in front of (further
 *      along the ground depth axis than) its parent's, clear of it;
 *   3. pedestal height rises with subtree size (dir-a holds dir-b, so
 *      dir-a must be taller);
 *   4. every child's wire starts on the parent's footprint edge and ends
 *      on the child's;
 *   5. file boxes stand inside their directory's pedestal top face.
 *
 * LINKING. geometry-fsn.c is compiled straight into this test (see
 * tests/meson.build) rather than reached through libfsvcore, which does
 * not carry it -- it calls gpu.h, exactly like geometry.c, and so belongs
 * to the frontends. That drags its *draw* pass's undefined references in
 * with it, hence the small block of gpu/text no-ops at the bottom of this
 * file: tools/fsv-headless-stubs.c cannot host them, because the two real
 * frontends both link that same renderer surface for real. Nothing here
 * calls them; they exist purely to satisfy the linker, and their being
 * unreachable from main( ) is itself the check that the layout pass draws
 * nothing.
 */

#include <assert.h>
#include <math.h>
#include <string.h>

#include "common.h" /* pulls in glib.h, and must precede it: G_LOG_DOMAIN */
#include "fsv.h"
#include "geometry-fsn.h"
#include "gpu.h"
#include "scanfs.h"
#include "tmaptext.h"

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

int
main(void)
{
	GNode *root, *dir_a, *dir_b, *file1;
	const FsnPedestal *p_root, *p_a, *p_b, *p_file1;
	FsnWire wire;
	double width, depth, height;

	scanfs(FIXTURE_DIR); /* FIXTURE_DIR injected by meson (-D) */
	assert(globals.fstree != NULL);

	root = root_dnode;
	assert(root != NULL);
	dir_a = child_named(root, "dir-a");
	assert(dir_a != NULL && NODE_IS_DIR(dir_a));
	dir_b = child_named(dir_a, "dir-b");
	assert(dir_b != NULL && NODE_IS_DIR(dir_b));
	file1 = child_named(root, "file1.txt");
	assert(file1 != NULL && !NODE_IS_DIR(file1));

	/* No layout yet */
	assert(fsn_layout_get(root) == NULL);

	fsn_geometry_init(root);

	p_root = fsn_layout_get(root);
	p_a = fsn_layout_get(dir_a);
	p_b = fsn_layout_get(dir_b);
	p_file1 = fsn_layout_get(file1);
	assert(p_root != NULL && p_a != NULL && p_b != NULL && p_file1 != NULL);

	/* 1. Root pedestal at the ground origin, with a real footprint */
	assert(fabs(p_root->x) < TOL);
	assert(fabs(p_root->z) < TOL);
	assert(p_root->w > 0.0 && p_root->d > 0.0 && p_root->h > 0.0);

	/* 2. dir-a in front of the root, clear of its footprint; and dir-b
	 * in front of dir-a in turn (distance grows per generation) */
	assert(p_a->z > p_root->z);
	assert((p_a->z - 0.5 * p_a->d) > (p_root->z + 0.5 * p_root->d));
	assert(p_b->z > p_a->z);
	assert((p_b->z - 0.5 * p_b->d) > (p_a->z + 0.5 * p_a->d));

	/* 3. Pedestal height monotonic in subtree size. dir-a's subtree
	 * strictly contains dir-b's (dir-b itself, plus file2.bin), so
	 * dir-a's pedestal must be the taller of the two. */
	assert(DIR_NODE_DESC(dir_a)->subtree.size >
	       DIR_NODE_DESC(dir_b)->subtree.size);
	assert(p_a->h > p_b->h);
	assert(p_root->h >= p_a->h);

	/* 4. Wires. The root has no parent wire; dir-a's runs from the root
	 * pedestal's outward edge to dir-a's inward edge, at the respective
	 * pedestal tops, and stays within both footprints' width. */
	assert(!fsn_layout_wire(root, &wire));
	assert(!fsn_layout_wire(file1, &wire));
	assert(fsn_layout_wire(dir_a, &wire));

	assert(fabs(wire.y0 - (p_root->z + 0.5 * p_root->d)) < TOL);
	assert(fabs(wire.x0 - p_root->x) <= 0.5 * p_root->w + TOL);
	assert(fabs(wire.z0 - p_root->h) < TOL);

	assert(fabs(wire.y1 - (p_a->z - 0.5 * p_a->d)) < TOL);
	assert(fabs(wire.x1 - p_a->x) <= 0.5 * p_a->w + TOL);
	assert(fabs(wire.z1 - p_a->h) < TOL);

	/* The wire spans the whole gap: it starts and ends nowhere else */
	assert(wire.y1 > wire.y0);

	/* 5. file1.txt's box stands entirely on the root's pedestal top */
	assert(p_file1->w > 0.0 && p_file1->d > 0.0 && p_file1->h > 0.0);
	assert((p_file1->x - 0.5 * p_file1->w) > (p_root->x - 0.5 * p_root->w));
	assert((p_file1->x + 0.5 * p_file1->w) < (p_root->x + 0.5 * p_root->w));
	assert((p_file1->z - 0.5 * p_file1->d) > (p_root->z - 0.5 * p_root->d));
	assert((p_file1->z + 0.5 * p_file1->d) < (p_root->z + 0.5 * p_root->d));

	/* Extents cover at least the root and the deepest child */
	fsn_layout_extents(&width, &depth, &height);
	assert(width >= p_root->w);
	assert(depth >= (p_b->z + 0.5 * p_b->d) - (p_root->z - 0.5 * p_root->d));
	assert(height >= p_root->h);

	fsn_geometry_free();
	assert(fsn_layout_get(root) == NULL);

	return 0;
}


/**** Link-only stubs -- see the note in this file's header ****/

FsvGpuMatrices gpu_mat;

void gpu_draw(FsvTopology topology, const FsvVertex *verts, int nverts,
              const unsigned int *indices, int nindices)
{
	(void)topology; (void)verts; (void)nverts;
	(void)indices; (void)nindices;
	g_assert_not_reached();
}

void gpu_upload_matrices(void) { g_assert_not_reached(); }
void gpu_set_color(float r, float g, float b, float a)
{
	(void)r; (void)g; (void)b; (void)a;
	g_assert_not_reached();
}
void gpu_set_lighting(int enabled) { (void)enabled; g_assert_not_reached(); }
void gpu_set_line_width(float width) { (void)width; g_assert_not_reached(); }
void gpu_set_depth_test(FsvDepthTest test) { (void)test; g_assert_not_reached(); }
FsvRenderMode gpu_render_mode(void) { g_assert_not_reached(); return FSV_RENDER_NORMAL; }

void text_pre(void) { g_assert_not_reached(); }
void text_post(void) { g_assert_not_reached(); }
void text_set_color(float r, float g, float b)
{
	(void)r; (void)g; (void)b;
	g_assert_not_reached();
}
void text_draw_straight(const char *text, const XYZvec *text_pos,
                        const XYvec *text_max_dims)
{
	(void)text; (void)text_pos; (void)text_max_dims;
	g_assert_not_reached();
}
