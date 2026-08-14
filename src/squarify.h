/* squarify.h — SPDX-License-Identifier: MIT
 *
 * Pure squarified-treemap layout (Bruls, Huizing, van Wijk 2000).
 * No glib, no gpu, no globals: this is libfsvcore's only fully
 * dependency-free module, which is what makes MapV's layout unit-
 * testable headlessly (geometry.c itself is GL-bound). */

#ifdef FSV_SQUARIFY_H
	#error
#endif
#define FSV_SQUARIFY_H

typedef struct {
	double x, y; /* origin corner */
	double w, h; /* extents, both > 0 */
} SquarifyRect;

/* Lays out n blocks with the given RELATIVE areas inside `bounds`:
 * the function normalizes internally so the rects tile bounds exactly.
 * out_rects (caller-allocated, n entries) is written in the INPUT's
 * order — internally the algorithm sorts descending by area (its
 * aspect-ratio guarantee needs that), but callers keep their own
 * node <-> rect correspondence by index. Zero/negative areas are
 * treated as a tiny epsilon share so every block gets a real rect. */
void squarify_layout( const SquarifyRect *bounds, const double *areas,
                      int n, SquarifyRect *out_rects );
