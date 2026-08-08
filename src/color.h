/* color.h */

/* Node coloration */

/* fsv - 3D File System Visualizer
 * Copyright (C)1999 Daniel Richard G. <skunk@mit.edu>
 *
 * SPDX-License-Identifier:  LGPL-2.1-or-later
 */


#ifdef FSV_COLOR_H
	#error
#endif
#define FSV_COLOR_H


/* The various coloring modes */
typedef enum {
	COLOR_BY_NODETYPE,
	COLOR_BY_TIMESTAMP,
	COLOR_BY_WPATTERN,
        COLOR_NONE
} ColorMode;

/* Every file has three timestamps */
typedef enum {
	TIMESTAMP_ACCESS, /* atime - time of last access */
	TIMESTAMP_MODIFY, /* mtime - time of last modification */
	TIMESTAMP_ATTRIB, /* ctime - time of last attribute change */
	TIMESTAMP_NONE
} TimeStampType;

/* Various kinds of spectrums */
typedef enum {
	SPECTRUM_RAINBOW,
        SPECTRUM_HEAT,
	SPECTRUM_GRADIENT,
	/* fsn-mode Task A2: the original fsn's 7-bucket "ages:" legend --
	 * absolute, fixed-cutoff age buckets (src/fsn-style.h's
	 * fsn_age_buckets[]), not a continuous windowed spectrum like the
	 * three above. Inserted here, before SPECTRUM_NONE, is safe on
	 * disk: color.c's nvstore round-trip persists SpectrumType by
	 * STRING token (tokens_timestamp_spectrum_type[]), never the raw
	 * enum int, so an existing ~/.fsvrc's "rainbow"/"heat"/"gradient"
	 * strings still resolve to the same values regardless of where a
	 * new enumerator lands. */
	SPECTRUM_FSN_BUCKETS,
	SPECTRUM_NONE
} SpectrumType;


/* Used indirectly in struct ColorConfig (see below) */
struct WPatternGroup {
	RGBcolor color;
	GList *wp_list; /* elements: char * */
};

struct ColorConfig {
	/* Node type colors */
	struct ColorByNodeType {
		RGBcolor colors[NUM_NODE_TYPES];
	} by_nodetype;

	/* Temporal spectrum type and range */
	struct ColorByTime {
		SpectrumType spectrum_type;
		TimeStampType timestamp_type;
		time_t new_time;
		time_t old_time;
		/* Following two are for gradient spectrums */
		RGBcolor old_color;
		RGBcolor new_color;
	} by_timestamp;

	/* Wildcard patterns */
	struct ColorByWPattern {
		GList *wpgroup_list; /* elements: struct WPatternGroup */
		RGBcolor default_color;
	} by_wpattern;
};


void color_config_destroy( struct ColorConfig *ccfg );
ColorMode color_get_mode( void );
void color_get_config( struct ColorConfig *ccfg );
/* fsn-mode Task A2: cheap peek at the live by_timestamp.spectrum_type,
 * for src/sdl/ui_rail.cpp's ui_legend_draw( ) to check every frame
 * without paying for a full color_get_config( )/color_config_destroy( )
 * deep copy (which also clones the by_wpattern group list) just to read
 * one enum field. Mirrors color_get_mode( )'s existing shape. */
SpectrumType color_timestamp_spectrum_type( void );
void color_assign_recursive( GNode *dnode );
void color_set_mode( ColorMode mode );
RGBcolor color_spectrum_color( SpectrumType type, double x, void *data );
void color_set_config( struct ColorConfig *new_ccfg, ColorMode mode );
void color_write_config( void );
void color_init( void );

/* Landscape (fsn-mode Task A1: sky/ground presets). Lives here rather
 * than in a new file because it is the exact same shape as the color
 * config above -- an nvstore-backed setting read once at startup and
 * written immediately on change -- and reuses the same open/close-per-
 * call pattern rather than sharing an NVStore handle across modules. See
 * src/fsn-style.h for the preset table (FsnLandscape, fsn_landscapes[])
 * this indexes into. */
int landscape_get( void );
void landscape_set( int index );
void landscape_init( void );

/* fsn-mode Task B3: whether the user has ever explicitly chosen a
 * landscape from the Display menu (landscape_set( ) above, its one and
 * only caller -- src/sdl/ui_main.cpp). Read by src/sdl/main.cpp's FSN
 * mode entry: it auto-selects "classic" (fsn-style.h's
 * FSN_LANDSCAPE_CLASSIC) through landscape_set_auto( ) below unless this
 * is already true, so a user who has never made an explicit choice sees
 * FSN's own default every time, but one who has always keeps it.
 * Persisted (nvstore key "landscape_explicit") so the distinction
 * survives a restart, same as the preset index itself. */
boolean landscape_explicit( void );

/* Same effect as landscape_set( ) -- applies the preset, persists the
 * raw "landscape" nvstore key, redraws -- but leaves the explicit flag
 * above untouched. The FSN auto-default's entry point; never called from
 * the Display menu. */
void landscape_set_auto( int index );


/* end color.h */
