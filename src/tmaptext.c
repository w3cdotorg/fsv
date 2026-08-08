/* tmaptext.c */

/* Texture-mapped text */

/* fsv - 3D File System Visualizer
 * Copyright (C)1999 Daniel Richard G. <skunk@mit.edu>
 * SPDX-FileCopyrightText: 2021 Janne Blomqvist <blomqvist.janne@gmail.com>
 *
 * SPDX-License-Identifier:  LGPL-2.1-or-later
 */

/* Ported to src/gpu.h for the SDL_GPU / Metal port (see docs/PORTING.md
 * Task 3.4): this file keeps the glyph-layout math it always had --
 * get_char_dims( ), get_char_tex_coords( ), and the three
 * text_draw_*( ) entry points are untouched apart from the vertex
 * struct now living in gpu.h (so both frontends agree on its layout).
 * What moved out is exactly the GL calls:
 * texture upload, shader program, and the VBO/EBO draw, which are now
 * gpu_text_init( )/gpu_text_draw( )/etc., implemented once per frontend
 * (src/sdl/gpu.cpp for SDL_GPU, src/ogl-gpu-compat.c for GTK/epoxy) --
 * the same split geometry.c went through in Task 3.3.
 *
 * Post-port (see docs/PORTING.md, "UTF-8 / accented characters"): the
 * font atlas itself moved out too, into fontatlas.c, and the strings
 * handed to text_draw_*( ) are now walked by Unicode codepoint rather
 * than by byte. Everything here is still fixed-cell monospace layout:
 * one codepoint is one cell, whatever the atlas source. */

#include "common.h"
#include "tmaptext.h"

#include "fontatlas.h"
#include "gpu.h"

/* Glyph cell dimensions. The atlas itself (and the codepoint -> cell
 * mapping) is fontatlas.c's business now: it either rasterizes a real
 * TrueType face or falls back to the ASCII-only XBM charset this file
 * used to embed directly, but either way the cells are this size and
 * laid out in rows of font_atlas_cols( ). */
#define char_width  FONT_CELL_WIDTH
#define char_height FONT_CELL_HEIGHT


/* Text can be squeezed to at most half its normal width */
#define TEXT_MAX_SQUEEZE 2.0


/* Normal character aspect ratio */
static const double char_aspect_ratio = (double)char_width / (double)char_height;

/* Atlas dimensions, as reported by font_atlas_build( ) */
static int charset_w = 1;
static int charset_h = 1;


/* Initializes texture-mapping state for drawing text */
void
text_init( void )
{
	byte *charset_pixels;

	/* Load texture. The old unpack-alignment, single-channel format,
	 * mipmap generation and sampler parameter calls all moved into
	 * gpu_text_init( ) -- see the note there and in docs/PORTING.md
	 * about the sampler differing (by design) between the two
	 * backends. */
	charset_pixels = font_atlas_build( &charset_w, &charset_h );
	gpu_text_init( charset_pixels, charset_w, charset_h );
	xfree( charset_pixels );
}


/* Decodes a UTF-8 string into one atlas cell index per *codepoint*
 * (not per byte -- the whole point of this: an accented character is
 * one glyph, and a codepoint the atlas doesn't have is one '?', not
 * one '?' per byte). Returns the number of cells, and stores a
 * caller-owned array of them in *cells.
 *
 * Filenames are arbitrary bytes, so this can and does get handed
 * invalid UTF-8 -- scanfs.c's display names are sanitized
 * (g_utf8_make_valid( )), but text_draw_*( ) is public API. Anything
 * that doesn't decode is consumed one byte at a time and drawn as the
 * '?' glyph, which both terminates and stays in step with the byte
 * count. */
static size_t
text_glyph_cells( const char *text, int **cells )
{
	const char *p;
	int *buf;
	size_t n = 0;

	buf = NEW_ARRAY(int, strlen( text ) + 1);
	for (p = text; *p != '\0'; ) {
		gunichar uc = g_utf8_get_char_validated( p, -1 );
		if ((uc == (gunichar)-1) || (uc == (gunichar)-2)) {
			/* Not valid UTF-8 -- one '?', one byte */
			buf[n++] = font_atlas_cell( '?' );
			++p;
		}
		else {
			buf[n++] = font_atlas_cell( uc );
			p = g_utf8_next_char( p );
		}
	}

	*cells = buf;

	return n;
}


/* Call before drawing text */
void
text_pre( void )
{
	gpu_text_begin( );
}


/* Call after drawing text */
void
text_post( void )
{
	gpu_text_end( );
}


/* Given the length of a string, and the dimensions into which that string
 * has to be rendered, this returns the dimensions that should be used
 * for each character */
static void
get_char_dims( int len, const XYvec *max_dims, XYvec *cdims )
{
	double min_width, max_width;

	/* Maximum and minimum widths of the string if it were to occupy
	 * the full depth (y-dimension) available to it */
	max_width = (double)len * max_dims->y * char_aspect_ratio;
	min_width = max_width / TEXT_MAX_SQUEEZE;

	if (max_width > max_dims->x) {
		if (min_width > max_dims->x) {
			/* Text will span full avaiable width, squeezed
			 * horizontally as much as it can take */
			cdims->x = max_dims->x / (double)len;
			cdims->y = TEXT_MAX_SQUEEZE * cdims->x / char_aspect_ratio;
		}
		else {
			/* Text will occupy full available width and
			 * height, squeezed horizontally a bit */
			cdims->x = max_dims->x / (double)len;
			cdims->y = max_dims->y;
		}
	}
	else {
		/* Text will use full available height (characters
		 * will have their natural aspect ratio) */
		cdims->y = max_dims->y;
		cdims->x = cdims->y * char_aspect_ratio;
	}
}


/* Returns the texture-space coordinates of the bottom-left and upper-right
 * corners of the specified glyph cell (as returned by font_atlas_cell( )) */
static void
get_char_tex_coords( int cell, XYvec *t_c0, XYvec *t_c1 )
{
	XYvec gpos;
	int cols;

	/* Get position of lower-left corner of glyph
	 * (in bitmap coordinates, w/origin at top-left) */
	cols = font_atlas_cols( );
	gpos.x = (double)((cell % cols) * char_width);
	gpos.y = (double)((cell / cols) * char_height);

	/* Texture coordinates */
	t_c0->x = gpos.x / (double)charset_w;
	t_c1->y = gpos.y / (double)charset_h;
	t_c1->x = t_c0->x + (double)char_width / (double)charset_w;
	t_c0->y = t_c1->y + (double)char_height / (double)charset_h;
}


/* Draw a set of text vertices with a specified color.
 * Use indexed drawing.
 * The vertices for each char must be in order
 * LL - LR - UL - UR */
static void
draw_text_vertices(FsvTextVertex *tv, size_t nchars)
{
	size_t idx_len = nchars * 6;
	unsigned int *idx;
	size_t i;

	idx = NEW_ARRAY(unsigned int, idx_len);
	for (i = 0; i < nchars; i++) {
		size_t j = 6 * i;  // 6 indices per char
		unsigned int v = 4 * i;  // 4 Vertices per char
		// First triangle in a character
		idx[j] = v;
		idx[j + 1] = v + 1;
		idx[j + 2] = v + 2;
		// Second triangle (counter-clockwise order)
		idx[j + 3] = v + 2;
		idx[j + 4] = v + 1;
		idx[j + 5] = v + 3;
	}

	gpu_text_draw(tv, nchars * 4, idx, idx_len);
	xfree(idx);
}

/* Draws a straight line of text centered at the given position,
 * fitting within the dimensions specified */
void
text_draw_straight( const char *text, const XYZvec *text_pos, const XYvec *text_max_dims )
{
	XYvec cdims;
	XYvec t_c0, t_c1, c0, c1;
	int *cells;
	size_t len;

	len = text_glyph_cells( text, &cells );
	get_char_dims( len, text_max_dims, &cdims );

	/* Corners of first character */
	c0.x = text_pos->x - 0.5 * (double)len * cdims.x;
	c0.y = text_pos->y - 0.5 * cdims.y;
	c1.x = c0.x + cdims.x;
	c1.y = c0.y + cdims.y;

	FsvTextVertex *tv = NEW_ARRAY(FsvTextVertex, len * 4);
	for (size_t i = 0; i < len; i++) {
		get_char_tex_coords( cells[i], &t_c0, &t_c1 );
		size_t j = i * 4;
		// Each char defined by corners in zigzag order
		// Lower left {pos, texcoords}
		tv[j] = (FsvTextVertex){{c0.x, c0.y, text_pos->z}, {t_c0.x, t_c0.y}};
		// Lower right
		tv[j + 1] = (FsvTextVertex){{c1.x, c0.y, text_pos->z}, {t_c1.x, t_c0.y}};
		// Upper left
		tv[j + 2] = (FsvTextVertex){{c0.x, c1.y, text_pos->z}, {t_c0.x, t_c1.y}};
		// Upper right
		tv[j + 3] = (FsvTextVertex){{c1.x, c1.y, text_pos->z}, {t_c1.x, t_c1.y}};

		c0.x = c1.x;
		c1.x += cdims.x;
	}
	draw_text_vertices(tv, len);
	xfree(tv);
	xfree(cells);
}


/* Draws a straight line of text centered at the given position, rotated
 * to be tangent to a circle around the origin, and fitting within the
 * dimensions specified (which are also rotated) */
void
text_draw_straight_rotated( const char *text, const RTZvec *text_pos, const XYvec *text_max_dims )
{
	XYvec cdims;
	XYvec t_c0, t_c1, c0, c1;
	XYvec hdelta, vdelta;
	double sin_theta, cos_theta;
	int *cells;
	size_t len;

	len = text_glyph_cells( text, &cells );
	get_char_dims( len, text_max_dims, &cdims );

	sin_theta = sin( RAD(text_pos->theta) );
	cos_theta = cos( RAD(text_pos->theta) );

	/* Vector to move from one character to the next */
	hdelta.x = sin_theta * cdims.x;
	hdelta.y = - cos_theta * cdims.x;
	/* Vector to move from bottom of character to top */
	vdelta.x = cos_theta * cdims.y;
	vdelta.y = sin_theta * cdims.y;

	/* Corners of first character */
	c0.x = cos_theta * text_pos->r - 0.5 * ((double)len * hdelta.x + vdelta.x);
	c0.y = sin_theta * text_pos->r - 0.5 * ((double)len * hdelta.y + vdelta.y);
	c1.x = c0.x + hdelta.x + vdelta.x;
	c1.y = c0.y + hdelta.y + vdelta.y;

	FsvTextVertex *tv = NEW_ARRAY(FsvTextVertex, len * 4);
	for (size_t i = 0; i < len; i++) {
		get_char_tex_coords( cells[i], &t_c0, &t_c1 );
		size_t j = i * 4;
		// Lower left
		tv[j] = (FsvTextVertex){{c0.x, c0.y, text_pos->z}, {t_c0.x, t_c0.y}};
		// Lower right
		tv[j + 1] = (FsvTextVertex){{c0.x + hdelta.x, c0.y + hdelta.y, text_pos->z},
					 {t_c1.x, t_c0.y}};
		// Upper left
		tv[j + 2] = (FsvTextVertex){{c1.x - hdelta.x, c1.y - hdelta.y, text_pos->z},
					 {t_c0.x, t_c1.y}};
		// Upper right
		tv[j + 3] = (FsvTextVertex){{c1.x, c1.y, text_pos->z}, {t_c1.x, t_c1.y}};

		c0.x += hdelta.x;
		c0.y += hdelta.y;
		c1.x += hdelta.x;
		c1.y += hdelta.y;
	}
	draw_text_vertices(tv, len);
	xfree(tv);
	xfree(cells);
}


/* Draws a curved arc of text, occupying no more than the depth and arc
 * width specified. text_pos indicates outer edge (not center) of text */
void
text_draw_curved( const char *text, const RTZvec *text_pos, const RTvec *text_max_dims )
{
	XYvec straight_dims, cdims;
	XYvec char_pos, fwsl, bwsl;
	XYvec t_c0, t_c1;
	double char_arc_width, theta;
	double sin_theta, cos_theta;
	double text_r;
	int *cells;
	size_t len;

	/* Convert curved dimensions to straight equivalent */
	straight_dims.x = (PI / 180.0) * text_pos->r * text_max_dims->theta;
	straight_dims.y = text_max_dims->r;

	len = text_glyph_cells( text, &cells );
	get_char_dims( len, &straight_dims, &cdims );

	/* Radius of center of text line */
	text_r = text_pos->r - 0.5 * cdims.y;

	/* Arc width occupied by each character */
	char_arc_width = (180.0 / PI) * cdims.x / text_r;

	theta = text_pos->theta + 0.5 * (double)(len - 1) * char_arc_width;

	FsvTextVertex *tv = NEW_ARRAY(FsvTextVertex, len * 4);
	for (size_t i = 0; i < len; i++) {
		sin_theta = sin( RAD(theta) );
		cos_theta = cos( RAD(theta) );

		/* Center of character and deltas from center to corners */
		char_pos.x = cos_theta * text_r;
		char_pos.y = sin_theta * text_r;
		/* "forward slash / backward slash" */
		fwsl.x = 0.5 * (cdims.y * cos_theta + cdims.x * sin_theta);
		fwsl.y = 0.5 * (cdims.y * sin_theta - cdims.x * cos_theta);
		bwsl.x = 0.5 * (- cdims.y * cos_theta + cdims.x * sin_theta);
		bwsl.y = 0.5 * (- cdims.y * sin_theta - cdims.x * cos_theta);

		get_char_tex_coords( cells[i], &t_c0, &t_c1 );
		size_t j = i * 4;
		// Lower left
		tv[j] = (FsvTextVertex){{char_pos.x - fwsl.x, char_pos.y - fwsl.y, text_pos->z},
				     {t_c0.x, t_c0.y}};
		// Lower right
		tv[j + 1] = (FsvTextVertex){{char_pos.x + bwsl.x, char_pos.y + bwsl.y, text_pos->z},
					 {t_c1.x, t_c0.y}};
		// Upper left
		tv[j + 2] = (FsvTextVertex){{char_pos.x - bwsl.x, char_pos.y - bwsl.y, text_pos->z},
					 {t_c0.x, t_c1.y}};
		// Upper right
		tv[j + 3] = (FsvTextVertex){{char_pos.x + fwsl.x, char_pos.y + fwsl.y, text_pos->z},
					 {t_c1.x, t_c1.y}};

		theta -= char_arc_width;
	}
	draw_text_vertices(tv, len);
	xfree(tv);
	xfree(cells);
}

// Set the text color
void
text_set_color(float red, float green, float blue)
{
	gpu_text_set_color(red, green, blue);
}


// Upload MVP matrix
void
text_upload_mvp(float* mvp)
{
	gpu_text_upload_mvp(mvp);
}

/* end tmaptext.c */
