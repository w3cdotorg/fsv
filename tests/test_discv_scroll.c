/* tests/test_discv_scroll.c
 *
 * SPDX-License-Identifier: MIT
 *
 * DiscV viewport scrolling (the upstream "TODO: To be implemented" pair in
 * src/camera.c): discv_get_scrollbar_state() must report a real
 * scrollable range -- the current disc's extent, MapV-convention signs
 * and margins -- instead of echoing back whatever scroll_state[] already
 * held, and discv_scrollbar_move() must consume what that state reports
 * (raw x, negated y -- the exact convention mapv_scrollbar_move()
 * established). Drives the real public entry points
 * (camera_update_scrollbars() -> fsv_platform.set_scroll capture,
 * camera_scrollbar_moved()) over a fixture scan whose DiscV geometry is
 * filled in by hand: geometry.c's discv_init() is frontend code
 * (GL-bound translation unit), so a headless test supplies the
 * DiscVGeomParams itself -- radius/pos are plain doubles in
 * NodeDesc::geomparams.
 *
 * LINKING: same recipe as test_fsn_camera (camera.c is in libfsvcore;
 * fsv_headless_platform_init() populates the platform table, and this
 * test then swaps in its own set_scroll to capture what camera.c
 * pushes). */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "fsv.h"
#include "camera.h"
#include "fsv-platform.h"
#include "geometry.h"
#include "scanfs.h"

void fsv_headless_platform_init( void );

static double cap_lower[2], cap_upper[2], cap_page[2], cap_value[2];
static int cap_calls;

/* camera_scrollbar_moved(axis) reads the dragged value back through
 * fsv_platform.get_scroll -- this feeds it. */
static double drag_value[2];

static double
feed_get_scroll(int axis)
{
	assert(axis == 0 || axis == 1);
	return drag_value[axis];
}

static void
capture_set_scroll(int axis, double lower, double upper, double page, double value)
{
	assert(axis == 0 || axis == 1);
	cap_lower[axis] = lower;
	cap_upper[axis] = upper;
	cap_page[axis] = page;
	cap_value[axis] = value;
	++cap_calls;
}

/* Fill a node's DiscV geometry by hand (see the header comment). */
static void
set_discv_geom(GNode *node, double radius, double px, double py)
{
	DiscVGeomParams *gp = DISCV_GEOM_PARAMS(node);

	gp->radius = radius;
	gp->theta = 0.0;
	gp->pos.x = px;
	gp->pos.y = py;
}

int
main(void)
{
	const double R = 100.0;
	GNode *node;
	double span_x, span_y, v0x, v0y;

	fsv_headless_platform_init();
	fsv_platform.set_scroll = capture_set_scroll;
	fsv_platform.get_scroll = feed_get_scroll;

	scanfs(FIXTURE_DIR);
	assert(globals.fstree != NULL && root_dnode != NULL);

	/* Hand-built DiscV layout: root disc of radius R at the origin;
	 * every other node small and centered (their geometry is irrelevant
	 * to the root's scroll range but must not be garbage --
	 * geometry_discv_node_pos() walks parents up to the metanode). */
	set_discv_geom(globals.fstree, 0.0, 0.0, 0.0);
	set_discv_geom(root_dnode, R, 0.0, 0.0);
	for (node = root_dnode->children; node != NULL; node = node->next)
		set_discv_geom(node, 1.0, 0.0, 0.0);

	globals.current_node = root_dnode;
	globals.fsv_mode = FSV_DISCV;
	camera_init(FSV_DISCV, TRUE);
	assert(camera->distance > 0.0);

	/* The stub this test buries: scroll_state[] starts zeroed, so the
	 * old echo behavior would report lower == upper == page == 0. */
	cap_calls = 0;
	camera_update_scrollbars(TRUE);
	assert(cap_calls == 2);

	/* A real range: the disc spans [-R, R] on both axes, plus the
	 * half-page corrective offsets MapV's convention adds on each end.
	 * Structurally: page > 0, range wider than one page, and the
	 * whole thing roughly centered on the disc. */
	span_x = cap_upper[0] - cap_lower[0];
	span_y = cap_upper[1] - cap_lower[1];
	assert(cap_page[0] > 0.0 && cap_page[1] > 0.0);
	assert(span_x > cap_page[0] - 1e-9);
	assert(span_y > cap_page[1] - 1e-9);
	assert(span_x <= 2.0 * R + 2.0 * cap_page[0] + 1e-9);
	/* Camera target at the origin: value sits mid-range on both axes
	 * (GtkAdjustment top-of-slider convention: mid == (lower + upper -
	 * page) / 2). */
	assert(fabs(cap_value[0] - 0.5 * (cap_lower[0] + cap_upper[0] - cap_page[0])) < 1e-6);
	assert(fabs(cap_value[1] - 0.5 * (cap_lower[1] + cap_upper[1] - cap_page[1])) < 1e-6);
	v0x = cap_value[0];
	v0y = cap_value[1];

	/* Moving the camera target moves the reported value with it --
	 * +x raw on the x axis, sign-reversed on y (MapV's canonical
	 * scrollbar-direction correction). */
	DISCV_CAMERA(camera)->target.x = 30.0;
	DISCV_CAMERA(camera)->target.y = 20.0;
	camera_update_scrollbars(TRUE);
	assert(fabs(cap_value[0] - (v0x + 30.0)) < 1e-6);
	assert(fabs(cap_value[1] - (v0y - 20.0)) < 1e-6);

	/* And the move side consumes exactly what the state reports:
	 * dragging to a value must land the target where a subsequent
	 * get reports that same value back (raw x, negated y). */
	drag_value[0] = v0x + 12.0;
	drag_value[1] = v0y - 5.0;
	camera_scrollbar_moved(0);
	camera_scrollbar_moved(1);
	assert(fabs(DISCV_CAMERA(camera)->target.x - 12.0) < 1e-6);
	assert(fabs(DISCV_CAMERA(camera)->target.y - 5.0) < 1e-6);

	printf("test_discv_scroll: OK\n");
	return 0;
}
