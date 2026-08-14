/* squarify.c — SPDX-License-Identifier: MIT
 * See squarify.h. Pure stdlib on purpose. */

#include <math.h>
#include <stdlib.h>

#include "squarify.h"

typedef struct {
	double area;
	int index; /* position in the caller's input */
} SqBlock;

/* Descending by area; ties broken by input order for determinism */
static int
sqblock_cmp( const void *va, const void *vb )
{
	const SqBlock *a = (const SqBlock *)va;
	const SqBlock *b = (const SqBlock *)vb;

	if (a->area > b->area)
		return -1;
	if (a->area < b->area)
		return 1;
	return a->index - b->index;
}

/* Worst aspect ratio of blocks [first..last] laid as one run of
 * thickness (run_area / side) along a side of length `side` */
static double
run_worst_aspect( const SqBlock *blocks, int first, int last,
    double side, double run_area )
{
	double thickness = run_area / side;
	double worst = 1.0;
	int i;

	for (i = first; i <= last; i++) {
		double len = blocks[i].area / thickness;
		double r = (len > thickness) ? (len / thickness)
		                             : (thickness / len);
		if (r > worst)
			worst = r;
	}
	return worst;
}

void
squarify_layout( const SquarifyRect *bounds, const double *areas, int n,
    SquarifyRect *out_rects )
{
	SqBlock *blocks;
	SquarifyRect free_rect;
	double total = 0.0, scale;
	int i, first;

	if (n <= 0)
		return;

	blocks = (SqBlock *)calloc( (size_t)n, sizeof(SqBlock) );
	for (i = 0; i < n; i++) {
		blocks[i].area = (areas[i] > 0.0) ? areas[i] : 0.0;
		blocks[i].index = i;
		total += blocks[i].area;
	}
	if (total <= 0.0) {
		/* Degenerate: nothing has area. Give every block an equal
		 * share rather than dividing by zero. */
		for (i = 0; i < n; i++)
			blocks[i].area = 1.0;
		total = (double)n;
	} else {
		/* A zero-area block among real ones still deserves a sliver
		 * (it must remain pickable): the smallest positive share */
		double epsilon = total * 1.0e-9;

		for (i = 0; i < n; i++)
			if (blocks[i].area <= 0.0) {
				blocks[i].area = epsilon;
				total += epsilon;
			}
	}

	qsort( blocks, (size_t)n, sizeof(SqBlock), sqblock_cmp );

	/* Normalize: block areas tile bounds exactly */
	scale = (bounds->w * bounds->h) / total;
	for (i = 0; i < n; i++)
		blocks[i].area *= scale;

	free_rect = *bounds;
	first = 0;
	while (first < n) {
		double side = (free_rect.w < free_rect.h) ? free_rect.w
		                                          : free_rect.h;
		double run_area = blocks[first].area;
		double worst = run_worst_aspect( blocks, first, first, side,
		    run_area );
		double thickness, along;
		int last = first;
		int is_final_run;

		/* Grow the run while the worst aspect ratio improves */
		while (last + 1 < n) {
			double try_area = run_area + blocks[last + 1].area;
			double try_worst = run_worst_aspect( blocks, first,
			    last + 1, side, try_area );

			if (try_worst > worst)
				break;
			last++;
			run_area = try_area;
			worst = try_worst;
		}

		is_final_run = (last == n - 1);

		/* Emit the run as one strip along the shorter side. The
		 * strip's thickness is derived from the REMAINING FREE RECT
		 * whenever this run empties it (the final run along the
		 * chosen axis, i.e. no free rect left over after it) rather
		 * than from run_area / side: run_area is itself a sum of
		 * scaled, floating-point areas, so run_area / side can miss
		 * free_rect.w or free_rect.h by a dust-sized epsilon. Since
		 * exact tiling is the contract (not just "close enough"),
		 * every strip that exhausts the free rect along its long
		 * axis must consume that axis exactly, by construction
		 * rather than by float luck. */
		along = 0.0;
		if (free_rect.w >= free_rect.h) {
			/* Wider than tall: strip on the left edge, blocks
			 * stacked in y */
			thickness = is_final_run ? free_rect.w
			                          : (run_area / side);
			for (i = first; i <= last; i++) {
				double len = blocks[i].area / thickness;
				SquarifyRect *r = &out_rects[blocks[i].index];

				r->x = free_rect.x;
				r->y = free_rect.y + along;
				r->w = thickness;
				r->h = len;
				along += len;
			}
			/* Stretch the run's last block to close any residual
			 * gap against the free rect's far edge in the strip
			 * direction (dust from summing blocks[i].area/thickness) */
			out_rects[blocks[last].index].h +=
			    (free_rect.y + free_rect.h)
			    - (out_rects[blocks[last].index].y
			       + out_rects[blocks[last].index].h);
			free_rect.x += thickness;
			free_rect.w -= thickness;
			if (is_final_run)
				free_rect.w = 0.0;
		} else {
			/* Taller than wide: strip on the bottom edge, blocks
			 * in x */
			thickness = is_final_run ? free_rect.h
			                          : (run_area / side);
			for (i = first; i <= last; i++) {
				double len = blocks[i].area / thickness;
				SquarifyRect *r = &out_rects[blocks[i].index];

				r->x = free_rect.x + along;
				r->y = free_rect.y;
				r->w = len;
				r->h = thickness;
				along += len;
			}
			out_rects[blocks[last].index].w +=
			    (free_rect.x + free_rect.w)
			    - (out_rects[blocks[last].index].x
			       + out_rects[blocks[last].index].w);
			free_rect.y += thickness;
			free_rect.h -= thickness;
			if (is_final_run)
				free_rect.h = 0.0;
		}
		first = last + 1;
	}

	free( blocks );
}
