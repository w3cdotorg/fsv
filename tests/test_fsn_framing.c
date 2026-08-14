/* tests/test_fsn_framing.c
 *
 * SPDX-License-Identifier: MIT
 *
 * Regression test for TODO.md bug #2: fsn_look_at( )'s file-zoom
 * framing computed its diameter from the target FILE's own pedestal
 * record -- the file box's own footprint, box-scale -- so a real
 * click-to-fly on a file in a densely packed directory put the camera
 * nose-first against the box row with no ground in frame (PORTING.md,
 * Task B3 verification: framing the camera on the PARENT directory
 * instead, selection left on the file, was the verified legible shot).
 *
 * The contract pinned here is exactly that verified framing, asserted
 * without duplicating any camera formula: looking at a file must
 * produce the SAME camera->distance as looking at its parent directory
 * (parent-footprint diameter rule, expanded-wire arm included), while
 * the camera target stays centered on the file's own box.
 *
 * MECHANICS. Camera pose changes are morphs, and no frames ever run
 * headlessly, so end values are snapped: morph_finish( ) zeroes a
 * morph's t_end and the next fsv_animation_tick( ) assigns its end
 * value synchronously (src/animation.c, morph_iteration( )).
 *
 * LINKING. Same recipe as test_fsn_camera: libfsvcore objects +
 * tools/fsv-headless-stubs.c (stateful dirtree flags, opt-in
 * fsv_platform no-op hooks). */

#include <assert.h>
#include <math.h>
#include <string.h>

#include "common.h" /* pulls in glib.h, and must precede it: G_LOG_DOMAIN */
#include "fsv.h"
#include "animation.h"
#include "camera.h"
#include "dirtree.h"
#include "fsv-platform.h"
#include "geometry-fsn.h"
#include "scanfs.h"

#define TOL 1.0e-9

/* tools/fsv-headless-stubs.c -- opt-in fsv_platform no-op hooks (the
 * shim has no header of its own) */
extern void fsv_headless_platform_init( void );

static GNode *
child_named(GNode *parent, const char *name)
{
	GNode *node;

	for (node = parent->children; node != NULL; node = node->next)
		if (strcmp(NODE_DESC(node)->name, name) == 0)
			return node;

	return NULL;
}

/* Snap the camera-pose morphs camera_look_at( ) schedules to their end
 * values: morph_finish( ) is a no-op for a variable not being morphed,
 * so finishing more than was scheduled is safe. */
static void
snap_camera_morphs(void)
{
	morph_finish(&camera->distance);
	morph_finish(&camera->theta);
	morph_finish(&camera->phi);
	morph_finish(&MAPV_CAMERA(camera)->target.x);
	morph_finish(&MAPV_CAMERA(camera)->target.y);
	morph_finish(&MAPV_CAMERA(camera)->target.z);
	fsv_animation_tick();
}

int
main(void)
{
	GNode *root, *dir_a, *file2;
	const FsnPedestal *file_ped;
	double dir_distance;

	fsv_headless_platform_init();

	scanfs(FIXTURE_DIR); /* FIXTURE_DIR injected by meson (-D) */
	assert(globals.fstree != NULL);

	root = root_dnode;
	assert(root != NULL);
	dir_a = child_named(root, "dir-a");
	assert(dir_a != NULL && NODE_IS_DIR(dir_a));
	file2 = child_named(dir_a, "file2.bin");
	assert(file2 != NULL && !NODE_IS_DIR(file2));

	globals.fsv_mode = FSV_FSN;
	fsn_geometry_init(root);
	camera_init(FSV_FSN, TRUE);

	/* The framing case users actually hit: the parent directory is
	 * expanded (its file boxes packed on the pedestal top) and a file
	 * box is clicked. Expanded also engages the wire arm of the
	 * directory diameter rule, so the parity assert below covers the
	 * whole rule, not just the SQRT_2 footprint half. */
	dirtree_entry_expand(dir_a);
	assert(dirtree_entry_expanded(dir_a));

	/* Establishing shot of the parent itself: the distance every
	 * fsn_look_at( ) caller gets for dir-a's own footprint. */
	camera_look_at(dir_a);
	snap_camera_morphs();
	dir_distance = camera->distance;
	assert(dir_distance > 0.0);

	/* Now the bug's exact gesture: click-to-fly on a file inside it. */
	camera_look_at(file2);
	snap_camera_morphs();

	/* Target stays centered on the file's own box... */
	file_ped = fsn_layout_get(file2);
	assert(file_ped != NULL);
	assert(fabs(MAPV_CAMERA(camera)->target.x - file_ped->x) < TOL);
	assert(fabs(MAPV_CAMERA(camera)->target.y - file_ped->z) < TOL);
	assert(fabs(MAPV_CAMERA(camera)->target.z -
	    (file_ped->h + fsn_layout_get(dir_a)->h)) < TOL);

	/* ...but the camera stands at the PARENT's establishing distance,
	 * not at the file box's own box-scale one. Pre-fix, this is the
	 * nose-first bug: distance derived from the file's tiny footprint,
	 * far short of the parent's. */
	assert(fabs(camera->distance - dir_distance) < TOL);

	return 0;
}
