/* stb_truetype.c */

/* The one translation unit that compiles stb_truetype's implementation.
 *
 * lib/stb_truetype.h is a verbatim copy of subprojects/imgui/
 * imstb_truetype.h (upstream stb_truetype.h v1.26 with Dear ImGui's
 * handful of warning fixes, all marked "[DEAR IMGUI]"), taken so that
 * src/fontatlas.c -- plain C, shared by both frontends -- can rasterize
 * glyphs without ever including an ImGui header. Both copies stay
 * independently updatable; see docs/PORTING.md ("UTF-8 / accented
 * characters").
 *
 * Public domain (or MIT), per the license block at the end of
 * stb_truetype.h itself.
 *
 * SPDX-License-Identifier:  MIT
 */

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

/* end stb_truetype.c */
