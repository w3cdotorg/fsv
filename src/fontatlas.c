/* fontatlas.c */

/* Glyph atlas for the texture-mapped 3D labels */

/* fsv - 3D File System Visualizer
 *
 * SPDX-License-Identifier:  LGPL-2.1-or-later
 */

/* Added after the Metal port (see docs/PORTING.md, "UTF-8 / accented
 * characters"). Upstream fsv had exactly one font: the 16x32 XBM
 * charset in src/xmaps/charset.xbm, 96 glyphs, ASCII 32..127 and
 * nothing else -- so "cosmique francais.mp3" spelled with a c-cedilla
 * came out as "cosmique franc??ais" (one '?' per UTF-8 *byte*).
 *
 * This file keeps that charset as a last-resort fallback and otherwise
 * rasterizes a real monospace TrueType face with stb_truetype into the
 * same fixed-cell layout, extended to cover Latin-1 Supplement, Latin
 * Extended-A and a handful of punctuation codepoints that turn up in
 * real filenames. The cell geometry (FONT_CELL_WIDTH x
 * FONT_CELL_HEIGHT, cells laid out left-to-right in rows of
 * ATLAS_COLS) is identical either way, so tmaptext.c's layout math --
 * and geometry.c's label fitting, which depends on the cell aspect
 * ratio -- is unaffected by which source produced the pixels.
 *
 * Deliberately free of any ImGui dependency: this is plain C compiled
 * into both frontends, and only lib/stb_truetype.h (a verbatim copy of
 * ImGui's vendored imstb_truetype.h) is shared with it. */

#include "common.h"
#include "fontatlas.h"

#include "stb_truetype.h"

/* The built-in fallback charset: 32 glyphs per row, ASCII 32..127 */
#include "xmaps/charset.xbm"


/* Glyph cells per atlas row. 32 is what the XBM charset has always
 * used (512px / 16px); keeping it means the generated atlas is the
 * same width, and only grows downward as coverage grows. */
#define ATLAS_COLS 32

/* Cell of the glyph every uncovered codepoint renders as */
#define FALLBACK_CODEPOINT '?'


typedef struct _CodepointRange CodepointRange;
struct _CodepointRange {
	gunichar first;
	gunichar last;
};

/* Coverage of the generated (stb_truetype) atlas. 336 cells total,
 * which fits in 11 rows of ATLAS_COLS. Latin Extended-A is the
 * "European filenames" range (it holds the Polish, Czech, Turkish,
 * Baltic and Esperanto letters, plus oe/OE ligatures); the two
 * punctuation entries cover the curly quotes, en/em dashes and the
 * euro sign that macOS and Windows both like to put in filenames. */
static const CodepointRange ttf_ranges[] = {
	{ 0x0020, 0x007E },	/* ASCII, printable */
	{ 0x00A0, 0x00FF },	/* Latin-1 Supplement */
	{ 0x0100, 0x017F },	/* Latin Extended-A */
	{ 0x2010, 0x201F },	/* dashes, curly quotes */
	{ 0x20AC, 0x20AC }	/* euro sign */
};

/* Coverage of the built-in XBM charset: one cell per code 32..127,
 * in code order -- the layout get_char_tex_coords( ) used to hardcode. */
static const CodepointRange xbm_ranges[] = {
	{ 0x0020, 0x007F }
};

/* Which of the two is live. Initialized to the fallback so that
 * font_atlas_cell( ) is answerable even before font_atlas_build( ). */
static const CodepointRange *atlas_ranges = xbm_ranges;
static int atlas_nranges = G_N_ELEMENTS(xbm_ranges);


/* Candidate typefaces, in preference order. All monospace: the cells
 * are fixed-width, so a proportional face would look wrong (and the
 * label-fitting math assumes a constant advance).
 *
 * The macOS pair is Courier New (a Supplemental font, present on every
 * stock install) then Menlo, which ships as a .ttc collection --
 * stbtt_GetFontOffsetForIndex( ) picks the face out of it. The Linux
 * entries cover the usual DejaVu/Liberation packaging paths across
 * Debian/Ubuntu, Fedora and Arch. All of them are listed on every
 * platform (an existence check is cheaper than an #ifdef maze, and a
 * macOS user with DejaVu installed under a Linux-ish prefix is welcome
 * to it). */
static const struct {
	const char *path;
	int index;		/* face index within a .ttc collection */
} font_candidates[] = {
	{ "/System/Library/Fonts/Supplemental/Courier New.ttf", 0 },
	{ "/System/Library/Fonts/Menlo.ttc", 0 },
	{ "/System/Library/Fonts/Supplemental/Andale Mono.ttf", 0 },
	{ "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 0 },
	{ "/usr/share/fonts/dejavu/DejaVuSansMono.ttf", 0 },
	{ "/usr/share/fonts/TTF/DejaVuSansMono.ttf", 0 },
	{ "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf", 0 },
	{ "/usr/share/fonts/liberation-mono/LiberationMono-Regular.ttf", 0 },
	{ "/usr/share/fonts/TTF/LiberationMono-Regular.ttf", 0 }
};

/* Result of font discovery (done once, then remembered) */
static boolean font_searched = FALSE;
static const char *font_path = NULL;
static int font_index = 0;


/* Simple XBM parser - bits to bytes. Caller assumes responsibility for
 * freeing the returned pixel buffer */
static byte *
xbm_pixels( const byte *xbm_bits, int pixel_count )
{
	int in_byte = 0;
	int bitmask = 1;
	int i;
	byte *pixels;

	pixels = NEW_ARRAY(byte, pixel_count);

	for (i = 0; i < pixel_count; i++) {
		/* Note: a 1 bit is black */
		if ((int)xbm_bits[in_byte] & bitmask)
			pixels[i] = 0;
		else
			pixels[i] = 255;

		if (bitmask & 128) {
			++in_byte;
			bitmask = 1;
		}
		else
			bitmask <<= 1;
	}

	return pixels;
}


/* Cell index of a codepoint within a range list, or -1 if uncovered */
static int
range_cell( const CodepointRange *ranges, int nranges, gunichar uc )
{
	int base = 0;
	int i;

	for (i = 0; i < nranges; i++) {
		if ((uc >= ranges[i].first) && (uc <= ranges[i].last))
			return base + (int)(uc - ranges[i].first);
		base += (int)(ranges[i].last - ranges[i].first) + 1;
	}

	return -1;
}


/* Total number of cells a range list needs */
static int
range_count( const CodepointRange *ranges, int nranges )
{
	int total = 0;
	int i;

	for (i = 0; i < nranges; i++)
		total += (int)(ranges[i].last - ranges[i].first) + 1;

	return total;
}


const char *
font_atlas_find_font( int *index )
{
	unsigned int i;

	if (!font_searched) {
		font_searched = TRUE;
		for (i = 0; i < G_N_ELEMENTS(font_candidates); i++) {
			if (g_file_test( font_candidates[i].path, G_FILE_TEST_IS_REGULAR )) {
				font_path = font_candidates[i].path;
				font_index = font_candidates[i].index;
				break;
			}
		}
		if (font_path == NULL)
			g_message( "fsv: no monospace TrueType font found; 3D labels fall back to the built-in ASCII charset (non-ASCII characters will render as '?')" );
	}

	if (index != NULL)
		*index = font_index;

	return font_path;
}


const char *
font_atlas_font_path( int *index )
{
	if (index != NULL)
		*index = font_index;

	return font_path;
}


/* Copies one rasterized glyph into its cell, clipped to the cell's own
 * rectangle (a glyph whose ink overflows the cell -- a tall accented
 * capital in a face with unusually tight vertical metrics -- must lose
 * the overflow rather than bleed into its neighbors) */
static void
blit_glyph( byte *atlas, int atlas_w, int cell_x, int cell_y,
	    const byte *glyph, int glyph_w, int glyph_h, int dx, int dy )
{
	int x, y;

	for (y = 0; y < glyph_h; y++) {
		int ay = cell_y + dy + y;
		if ((ay < cell_y) || (ay >= cell_y + FONT_CELL_HEIGHT))
			continue;
		for (x = 0; x < glyph_w; x++) {
			int ax = cell_x + dx + x;
			if ((ax < cell_x) || (ax >= cell_x + FONT_CELL_WIDTH))
				continue;
			atlas[ay * atlas_w + ax] = glyph[y * glyph_w + x];
		}
	}
}


/* Rasterizes ttf_ranges out of a scalable font. Returns NULL (leaving
 * the width/height outputs alone) if the font can't be read or parsed */
static byte *
build_from_font( const char *path, int index, int *width, int *height )
{
	stbtt_fontinfo font;
	char *data = NULL;
	gsize data_len = 0;
	byte *atlas;
	int ncells, nrows, atlas_w, atlas_h;
	int offset, ascent, descent, line_gap, advance, lsb;
	int baseline, cell, i;
	float scale, text_height;
	gunichar uc;

	if (!g_file_get_contents( path, &data, &data_len, NULL ))
		return NULL;

	offset = stbtt_GetFontOffsetForIndex( (const unsigned char *)data, index );
	if ((offset < 0) || !stbtt_InitFont( &font, (const unsigned char *)data, offset )) {
		g_free( data );
		return NULL;
	}

	/* Scale: fit one character advance into the cell width, which is
	 * what makes the generated atlas drop-in-compatible with the XBM
	 * one (same 1:2 cell aspect ratio, same "every glyph is one cell
	 * wide" assumption in tmaptext.c). 'M' stands in for the advance
	 * of the whole face -- these are all monospace fonts, so any
	 * glyph would do. */
	stbtt_GetCodepointHMetrics( &font, 'M', &advance, &lsb );
	if (advance <= 0) {
		g_free( data );
		return NULL;
	}
	scale = (float)FONT_CELL_WIDTH / (float)advance;

	/* ...then shrink further if the face is tall enough that
	 * ascender-to-descender would not fit the cell height. */
	stbtt_GetFontVMetrics( &font, &ascent, &descent, &line_gap );
	text_height = (float)(ascent - descent) * scale;
	if (text_height > (float)FONT_CELL_HEIGHT) {
		scale *= (float)FONT_CELL_HEIGHT / text_height;
		text_height = (float)FONT_CELL_HEIGHT;
	}
	/* Vertically center the ascender/descender band in the cell */
	baseline = (int)(0.5f * ((float)FONT_CELL_HEIGHT - text_height)
			 + (float)ascent * scale + 0.5f);

	ncells = range_count( ttf_ranges, G_N_ELEMENTS(ttf_ranges) );
	nrows = (ncells + ATLAS_COLS - 1) / ATLAS_COLS;
	atlas_w = ATLAS_COLS * FONT_CELL_WIDTH;
	atlas_h = nrows * FONT_CELL_HEIGHT;
	atlas = NEW_ARRAY(byte, atlas_w * atlas_h);
	memset( atlas, 0, (size_t)atlas_w * (size_t)atlas_h );

	cell = 0;
	for (i = 0; i < (int)G_N_ELEMENTS(ttf_ranges); i++)
	for (uc = ttf_ranges[i].first; uc <= ttf_ranges[i].last; uc++, cell++) {
		int glyph, x0, y0, x1, y1, glyph_w, glyph_h, pen_x;
		byte *bitmap;

		glyph = stbtt_FindGlyphIndex( &font, (int)uc );
		if (glyph == 0) {
			/* Font doesn't have it: show the same '?' every
			 * uncovered codepoint gets, rather than a blank */
			glyph = stbtt_FindGlyphIndex( &font, FALLBACK_CODEPOINT );
			if (glyph == 0)
				continue;
		}

		stbtt_GetGlyphBitmapBox( &font, glyph, scale, scale, &x0, &y0, &x1, &y1 );
		glyph_w = x1 - x0;
		glyph_h = y1 - y0;
		if ((glyph_w <= 0) || (glyph_h <= 0))
			continue; /* whitespace */

		bitmap = NEW_ARRAY(byte, glyph_w * glyph_h);
		stbtt_MakeGlyphBitmap( &font, bitmap, glyph_w, glyph_h, glyph_w, scale, scale, glyph );

		/* Center the glyph's advance width in the cell (the XBM
		 * charset's glyphs are centered too) */
		stbtt_GetGlyphHMetrics( &font, glyph, &advance, &lsb );
		pen_x = (FONT_CELL_WIDTH - (int)((float)advance * scale + 0.5f)) / 2;

		blit_glyph( atlas, atlas_w,
			    (cell % ATLAS_COLS) * FONT_CELL_WIDTH,
			    (cell / ATLAS_COLS) * FONT_CELL_HEIGHT,
			    bitmap, glyph_w, glyph_h,
			    pen_x + x0, baseline + y0 );
		xfree( bitmap );
	}

	g_free( data );

	*width = atlas_w;
	*height = atlas_h;

	return atlas;
}


byte *
font_atlas_build( int *width, int *height )
{
	const char *path;
	byte *pixels;
	int index;

	path = font_atlas_find_font( &index );
	if (path != NULL) {
		pixels = build_from_font( path, index, width, height );
		if (pixels != NULL) {
			atlas_ranges = ttf_ranges;
			atlas_nranges = G_N_ELEMENTS(ttf_ranges);
			g_message( "fsv: 3D label glyphs rasterized from %s (%d x %d atlas)", path, *width, *height );
			return pixels;
		}
		g_warning( "fsv: %s could not be rasterized; 3D labels fall back to the built-in ASCII charset", path );
		font_path = NULL; /* so the ImGui panels fall back in step */
	}

	/* Built-in charset: ASCII only, exactly as upstream fsv had it */
	atlas_ranges = xbm_ranges;
	atlas_nranges = G_N_ELEMENTS(xbm_ranges);
	*width = charset_width;
	*height = charset_height;

	return xbm_pixels( charset_bits, charset_width * charset_height );
}


int
font_atlas_cols( void )
{
	return ATLAS_COLS;
}


int
font_atlas_cell( gunichar uc )
{
	int cell;

	cell = range_cell( atlas_ranges, atlas_nranges, uc );
	if (cell < 0)
		cell = range_cell( atlas_ranges, atlas_nranges, FALLBACK_CODEPOINT );

	return MAX(cell, 0);
}


/* end fontatlas.c */
