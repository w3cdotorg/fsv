/* tests/test_camera_theta.c
 *
 * SPDX-License-Identifier: MIT
 *
 * Short-arc theta on fixed-heading re-poses (TODO.md's last Known bug):
 * camera_revolve( ) accumulates camera->theta without wrapping, and
 * morph( ) interpolates raw numbers, not angles -- so a bird's-eye hop
 * (or a look-at) after a manual revolve used to spin the long way
 * round to its fixed heading in DiscV/MapV/TreeV. FSV_FSN got
 * unwrap_theta_toward( ) in fsn-mode Task B2; this locks the same
 * treatment onto the other modes' re-pose sites.
 *
 * The revolve is simulated by assigning camera->theta directly (the
 * accumulation is the documented behavior under test's PREcondition,
 * not its subject), morphs are snapped via morph_finish( ) +
 * fsv_animation_tick( ) (same mechanics as test_fsn_framing), and the
 * assertion is on observable state only: the settled heading is
 * congruent to the target mod 360, and the total theta travel of the
 * morph never exceeded 180 degrees.
 *
 * LINKING: the standard libfsvcore + headless-stubs recipe. MapV's
 * geometry params are all zero headlessly (geometry.c never ran);
 * every quantity the exercised paths derive from them stays finite
 * (field_distance( ) is floored), and theta -- the subject -- does not
 * depend on layout at all. */

#include <assert.h>
#include <math.h>

#include "common.h" /* pulls in glib.h, and must precede it: G_LOG_DOMAIN */
#include "fsv.h"
#include "animation.h"
#include "camera.h"
#include "dirtree.h"
#include "fsv-platform.h"
#include "scanfs.h"

#define TOL 1.0e-9

/* tools/fsv-headless-stubs.c -- opt-in fsv_platform no-op hooks */
extern void fsv_headless_platform_init( void );

static void
snap_theta_morph(void)
{
	morph_finish(&camera->theta);
	morph_finish(&camera->phi);
	morph_finish(&camera->distance);
	morph_finish(&camera->near_clip);
	morph_finish(&camera->far_clip);
	morph_finish(&camera->pan_part);
	fsv_animation_tick();
}

/* Smallest congruence distance between two headings, in degrees */
static double
angle_diff(double a, double b)
{
	double d = fmod(fabs(a - b), 360.0);

	return d > 180.0 ? 360.0 - d : d;
}

/* One scenario: in `mode`, park the camera at a wound-up heading
 * (simulating several manual revolves), fire the bird's-eye hop, snap,
 * and require (1) the settled heading is the mode's fixed bird's-eye
 * target mod 360 and (2) the morph's endpoints were never more than
 * 180 apart -- the short arc. Then return from bird's-eye and require
 * the same short-arc property on the restore. */
static void
check_birdseye_short_arc(FsvMode mode, double wound_theta,
    double expect_target_mod360)
{
	double before, after;

	globals.fsv_mode = mode;
	camera_init(mode, TRUE);
	snap_theta_morph(); /* settle any init pan */

	camera->theta = wound_theta;

	camera_birdseye_view(TRUE);
	before = camera->theta; /* unwrap may have adjusted it in place */
	snap_theta_morph();
	after = camera->theta;

	assert(angle_diff(after, expect_target_mod360) < TOL);
	assert(fabs(after - before) <= 180.0 + TOL);

	/* And back down: the restore must also take the short arc to the
	 * saved pre-bird's-eye pose (wound_theta itself, mod 360) */
	camera_birdseye_view(FALSE);
	before = camera->theta;
	snap_theta_morph();
	after = camera->theta;

	assert(angle_diff(after, wound_theta) < TOL);
	assert(fabs(after - before) <= 180.0 + TOL);
}

int
main(void)
{
	fsv_headless_platform_init();

	scanfs(FIXTURE_DIR);
	assert(globals.fstree != NULL && root_dnode != NULL);

	/* A real session always has a current node (fsv.c's startup call
	 * to camera_look_at_full( root_dnode, ... )) before anything can
	 * touch bird's-eye view; a fresh headless scan does not establish
	 * one on its own. Set it directly rather than routing through
	 * camera_look_at( ) (which would additionally exercise the pan
	 * machinery this test is not about): camera_birdseye_view( )'s
	 * going-down restore ends in post_pan_end( ) ->
	 * camera_update_scrollbars( ) -> mapv_get_scrollbar_state( ), which
	 * dereferences globals.current_node->parent once bird's-eye is no
	 * longer active -- a NULL current_node crashes there, not on the
	 * short-arc assertion under test. */
	globals.current_node = root_dnode;

	/* MapV bird's-eye heading is 270; park at 630 (= 270 + 360): the
	 * long way round is a full -360 sweep, the short arc is zero. */
	check_birdseye_short_arc(FSV_MAPV, 630.0, 270.0);

	/* And from a heading NOT congruent to the target: 610 -> nearest
	 * 270-congruent value is 630, a +20 short arc (the old code went
	 * 610 -> 270, a -340 sweep). */
	check_birdseye_short_arc(FSV_MAPV, 610.0, 270.0);

	/* TreeV: its bird's-eye heading depends on the camera's own
	 * target.theta (90 - target.theta); read the expectation from the
	 * live camera rather than hardcoding it. */
	{
		double expect;

		globals.fsv_mode = FSV_TREEV;
		camera_init(FSV_TREEV, TRUE);
		snap_theta_morph();
		expect = 90.0 - TREEV_CAMERA(camera)->target.theta;
		camera->theta = expect + 360.0 + 20.0; /* wound up a turn + 20 */

		camera_birdseye_view(TRUE);
		{
			double before = camera->theta, after;

			snap_theta_morph();
			after = camera->theta;
			assert(angle_diff(after, expect) < TOL);
			assert(fabs(after - before) <= 180.0 + TOL);
		}
		camera_birdseye_view(FALSE);
		snap_theta_morph();
	}

	return 0;
}
