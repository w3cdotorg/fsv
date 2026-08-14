/* tests/test_squarify.c
 *
 * SPDX-License-Identifier: MIT
 *
 * Invariants of the pure squarified-treemap module (src/squarify.c),
 * the replacement for geometry.c's 1999 greedy full-width-row MapV
 * layout ("some nodes too wide, some too tall, frontmost rows
 * paper-thin" -- the upstream TODO's own words):
 *
 *   1. exact tiling: rect areas sum to bounds' area;
 *   2. containment: every rect inside bounds (within TOL);
 *   3. no overlaps: pairwise intersection area is zero (within TOL);
 *   4. input-order correspondence: biggest input area gets the
 *      biggest rect, and each out_rects[i] area is proportional to
 *      areas[i];
 *   5. the point of it all: on a byte-skewed distribution (one huge
 *      block + many small ones -- the .git case), the worst aspect
 *      ratio beats the old algorithm's full-width-rows result,
 *      recomputed here as the reference. */

#include <assert.h>
#include <math.h>

#include "squarify.h"

#define TOL 1.0e-6
#define N_SKEW 65

static double
rect_area(const SquarifyRect *r)
{
	return r->w * r->h;
}

static double
overlap_1d(double a0, double a1, double b0, double b1)
{
	double lo = a0 > b0 ? a0 : b0;
	double hi = a1 < b1 ? a1 : b1;
	return hi > lo ? hi - lo : 0.0;
}

static double
worst_aspect(const SquarifyRect *rects, int n)
{
	double worst = 1.0;
	int i;

	for (i = 0; i < n; i++) {
		double r = rects[i].w / rects[i].h;
		if (r < 1.0)
			r = 1.0 / r;
		if (r > worst)
			worst = r;
	}
	return worst;
}

/* The OLD algorithm, reduced to its aspect-ratio-relevant core (from
 * geometry.c's pass 2/4 before this change): greedy rows spanning the
 * FULL bounds width; a row closes when its newest block's aspect
 * (width/depth) drops below 1. Returns the worst aspect ratio it
 * would produce for the given areas. */
static double
old_rows_worst_aspect(const SquarifyRect *bounds, const double *areas,
    int n)
{
	double total = 0.0, scale;
	double row_area = 0.0, worst = 1.0;
	double bw = bounds->w;
	int i, row_start = 0;

	for (i = 0; i < n; i++)
		total += areas[i];
	scale = (bounds->w * bounds->h) / total;

	for (i = 0; i < n; i++) {
		double a = areas[i] * scale;
		double depth, width, r;
		int j;

		row_area += a;
		depth = row_area / bw;
		width = a / depth;
		if (width / depth < 1.0 || i == n - 1) {
			/* Row closes: score every block in it */
			for (j = row_start; j <= i; j++) {
				width = areas[j] * scale / depth;
				r = width / depth;
				if (r < 1.0)
					r = 1.0 / r;
				if (r > worst)
					worst = r;
			}
			row_area = 0.0;
			row_start = i + 1;
		}
	}
	return worst;
}

int
main(void)
{
	SquarifyRect bounds;
	SquarifyRect rects[N_SKEW];
	double areas[N_SKEW];
	double sum, worst_new, worst_old;
	int i, j;

	bounds.x = -320.0;
	bounds.y = 100.0;
	bounds.w = 1200.0;
	bounds.h = 1000.0;

	/* The .git case: one block holding ~94% of the total, 64 small
	 * ones sharing the rest (with some variety) */
	areas[0] = 100000.0;
	for (i = 1; i < N_SKEW; i++)
		areas[i] = 50.0 + 7.0 * (double)(i % 9);

	squarify_layout(&bounds, areas, N_SKEW, rects);

	/* 1. exact tiling */
	sum = 0.0;
	for (i = 0; i < N_SKEW; i++)
		sum += rect_area(&rects[i]);
	assert(fabs(sum - bounds.w * bounds.h) < TOL * bounds.w * bounds.h);

	/* 2. containment */
	for (i = 0; i < N_SKEW; i++) {
		assert(rects[i].w > 0.0 && rects[i].h > 0.0);
		assert(rects[i].x >= bounds.x - TOL);
		assert(rects[i].y >= bounds.y - TOL);
		assert(rects[i].x + rects[i].w <= bounds.x + bounds.w + TOL);
		assert(rects[i].y + rects[i].h <= bounds.y + bounds.h + TOL);
	}

	/* 3. no overlaps */
	for (i = 0; i < N_SKEW; i++)
		for (j = i + 1; j < N_SKEW; j++) {
			double ox = overlap_1d(rects[i].x, rects[i].x + rects[i].w,
			    rects[j].x, rects[j].x + rects[j].w);
			double oy = overlap_1d(rects[i].y, rects[i].y + rects[i].h,
			    rects[j].y, rects[j].y + rects[j].h);
			assert(ox * oy < TOL * bounds.w * bounds.h);
		}

	/* 4. order correspondence: out_rects[i] area proportional to
	 * areas[i] (same normalization for all), so the huge input is the
	 * huge rect */
	{
		double unit = rect_area(&rects[1]) / areas[1];

		for (i = 0; i < N_SKEW; i++)
			assert(fabs(rect_area(&rects[i]) - areas[i] * unit)
			    < 1.0e-3 * areas[0] * unit);
		assert(rect_area(&rects[0]) > rect_area(&rects[1]));
	}

	/* 5. the improvement that motivates the rewrite */
	worst_new = worst_aspect(rects, N_SKEW);
	worst_old = old_rows_worst_aspect(&bounds, areas, N_SKEW);
	assert(worst_new < worst_old);
	/* And not merely "less bad": squarify's blocks stay recognizably
	 * block-like even here */
	assert(worst_new < 8.0);

	/* Degenerate input: all-zero areas must still tile (equal split),
	 * not crash or divide by zero */
	{
		double zeros[4] = { 0.0, 0.0, 0.0, 0.0 };
		SquarifyRect zrects[4];

		squarify_layout(&bounds, zeros, 4, zrects);
		sum = 0.0;
		for (i = 0; i < 4; i++)
			sum += rect_area(&zrects[i]);
		assert(fabs(sum - bounds.w * bounds.h) < TOL * bounds.w * bounds.h);
	}

	return 0;
}
