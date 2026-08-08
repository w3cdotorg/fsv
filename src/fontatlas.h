/* fontatlas.h */

/* Glyph atlas for the texture-mapped 3D labels (see tmaptext.c) */

/* fsv - 3D File System Visualizer
 *
 * SPDX-License-Identifier:  LGPL-2.1-or-later
 */

#ifndef FSV_FONTATLAS_H
#define FSV_FONTATLAS_H

/* <glib.h>, not common.h: this header is included from src/sdl/main.cpp
 * too, and common.h has no include guard of its own (including it twice
 * in one translation unit is a hard error). Only glib types appear
 * below -- guint8 is exactly common.h's `byte`. */
#include <glib.h>

/* Every glyph occupies a fixed cell of this size, whichever atlas
 * source is in use. tmaptext.c's layout math (and, through
 * char_aspect_ratio, geometry.c's label fitting) depends on the ratio
 * of the two, so it is deliberately the same 16x32 the original XBM
 * charset used. */
#define FONT_CELL_WIDTH		16
#define FONT_CELL_HEIGHT	32

/* Builds the glyph atlas -- once; repeated calls return a fresh copy of
 * the same pixels. The returned buffer is a width x height array of
 * 8-bit coverage values (255 == ink), i.e. exactly what gpu_text_init( )
 * wants; the caller owns it and must xfree( ) it.
 *
 * Never fails: if no scalable font can be found, this falls back to the
 * built-in ASCII-only XBM charset (and says so, once). */
guint8 *font_atlas_build( int *width, int *height );

/* Number of glyph cells per atlas row */
int font_atlas_cols( void );

/* Cell index of a Unicode codepoint. Codepoints the atlas does not
 * cover -- including everything outside Latin/Latin-1/Latin Extended-A
 * when the XBM fallback is in use -- map to the single '?' cell. */
int font_atlas_cell( gunichar uc );

/* Path of the scalable font the atlas was built from, or NULL if the
 * XBM fallback is in use. For .ttc collections, *index receives the
 * face index within the collection (0 otherwise). Only meaningful
 * after font_atlas_build( ); the SDL frontend reuses it so the ImGui
 * panels and the 3D labels always agree on the typeface. */
const char *font_atlas_font_path( int *index );

/* Runs font discovery without building anything, so a caller that only
 * wants the font (src/sdl/main.cpp's ImGui setup) does not depend on
 * text_init( ) having run first. Same return convention as
 * font_atlas_font_path( ). */
const char *font_atlas_find_font( int *index );

#endif /* not FSV_FONTATLAS_H */

/* end fontatlas.h */
