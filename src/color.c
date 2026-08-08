/* color.c */

/* Node coloration */

/* fsv - 3D File System Visualizer
 * Copyright (C)1999 Daniel Richard G. <skunk@mit.edu>
 *
 * SPDX-License-Identifier:  LGPL-2.1-or-later
 */


#include "common.h"
#include "color.h"

#include <fnmatch.h>
#include <time.h>
#include "nvstore.h"

#include "animation.h" /* redraw( ) */
#include "geometry.h"
#include "gpu.h" /* gpu_set_landscape( ) */
#include "window.h"

#include "fsn-style.h" /* FsnLandscape, fsn_landscapes[], FSN_LANDSCAPE_COUNT */


/* Some fnmatch headers don't define FNM_FILE_NAME */
/* (*cough*Solaris*cough*) */
#ifndef FNM_FILE_NAME
	#define FNM_FILE_NAME FNM_PATHNAME
#endif

/* Number of shades in a spectrum */
#define SPECTRUM_NUM_SHADES 1024


/* Default configuration */
static const ColorMode default_color_mode = COLOR_BY_NODETYPE;
static const char *default_nodetype_colors[NUM_NODE_TYPES] = {
	NULL,		/* Metanode (not used) */
	"#A0A0A0",	/* Directory */
	"#FFFFA0",	/* Regular file */
	"#FFFFFF",	/* Symlink */
	"#00FF00",	/* FIFO */
	"#FF8000",	/* Socket */
	"#00FFFF",	/* Character device */
	"#4CA0FF",	/* Block device */
	"#FF0000"	/* unknown */
};
static const int default_timestamp_spectrum_type = SPECTRUM_RAINBOW;
static const int default_timestamp_timestamp_type = TIMESTAMP_MODIFY;
static const int default_timestamp_period = 7 * 24 * 60 * 60; /* 1 week */
static const char default_timestamp_old_color[] = "#0000FF";
static const char default_timestamp_new_color[] = "#FF0000";
// static const char *default_wpattern_groups[] = {
// 	"#FF3333", "*.arj", "*.gz", "*.lzh", "*.tar", "*.tgz", "*.z", "*.zip", "*.Z", NULL,
// 	"#FF33FF", "*.gif", "*.jpg", "*.png", "*.ppm", "*.tga", "*.tif", "*.xpm", NULL,
// 	"#FFFFFF", "*.au", "*.mov", "*.mp3", "*.mpg", "*.wav", NULL,
// 	NULL
// };
static const char default_wpattern_default_color[] = "#FFFFA0";

/* For configuration file: key and token strings */
static const char key_color[] = "color";
static const char key_color_mode[] = "colormode";
static const char *tokens_color_mode[] = {
	"nodetype",
	"time",
	"wpattern",
	NULL
};
static const char key_nodetype[] = "nodetype";
static const char *keys_nodetype_node_type[NUM_NODE_TYPES] = {
	NULL,
	"directory",
	"regularfile",
	"symlink",
	"pipe",
	"socket",
	"chardevice",
	"blockdevice",
	"unknown"
};
static const char key_timestamp[] = "timestamp";
static const char key_timestamp_spectrum_type[] = "spectrumtype";
static const char *tokens_timestamp_spectrum_type[] = {
	"rainbow",
	"heat",
	"gradient",
	"fsnbuckets", /* fsn-mode Task A2: SPECTRUM_FSN_BUCKETS -- index
	               * must match that enumerator's position in
	               * color.h's SpectrumType, same hand-kept-in-sync
	               * convention as every other token array here */
	NULL
};
static const char key_timestamp_timestamp_type[] = "timestamptype";
static const char *tokens_timestamp_timestamp_type[] = {
	"access",
	"modify",
	"attribchange",
	NULL
};
static const char key_timestamp_period[] = "period";
static const char key_timestamp_old_color[] = "oldcolor";
static const char key_timestamp_new_color[] = "newcolor";
static const char key_wpattern[] = "wpattern";
static const char key_wpattern_group[] = "group";
static const char key_wpattern_group_color[] = "color";
static const char key_wpattern_group_wpattern[] = "wp";
static const char key_wpattern_default_color[] = "defaultcolor";

/* Landscape (fsn-mode Task A1). One int-token key, same shape as
 * key_color_mode above -- tokens_landscape[]'s order must match
 * src/fsn-style.h's fsn_landscapes[] order (index-for-index), same
 * hand-kept-in-sync convention tokens_color_mode already relies on for
 * ColorMode. */
static const char key_landscape[] = "landscape";
static const char *tokens_landscape[] = {
	"classic",
	"night",
	"slate",
	NULL
};
/* "slate" (index 2): matches today's pre-A1 flat clear -- see
 * fsn-style.h -- so an existing ~/.fsvrc with no `landscape` key at all
 * (every install before this task) keeps its exact current look. */
static const int default_landscape = 2;

/* fsn-mode Task B3: has the user ever chosen a landscape explicitly?
 * See color.h's landscape_explicit( ) doc comment. */
static const char key_landscape_explicit[] = "landscape_explicit";

/* Color configuration */
static struct ColorConfig color_config;

/* Color assignment mode */
static ColorMode color_mode;

/* Current landscape preset (index into fsn_landscapes[], or
 * FSN_LANDSCAPE_OFF). Mirrors what was last handed to
 * gpu_set_landscape() -- kept here too (rather than read back from
 * gpu.h, which has no getter) purely so landscape_write_config() and
 * src/sdl/ui_main.cpp's menu have something to read without adding one. */
static int landscape_current = -1;

/* fsn-mode Task B3: mirrors what was last read from/written to the
 * "landscape_explicit" nvstore key -- see color.h's landscape_explicit( )
 * doc comment. */
static boolean landscape_explicit_current = FALSE;

/* Colors for spectrum */
static RGBcolor spectrum_underflow_color;
static RGBcolor spectrum_colors[SPECTRUM_NUM_SHADES];
static RGBcolor spectrum_overflow_color;


/* Copies a ColorConfig structure from one location to another */
static void
color_config_copy( struct ColorConfig *to, struct ColorConfig *from )
{
	struct WPatternGroup *wpgroup, *new_wpgroup;
	GList *wpgroup_llink, *wp_llink;
	char *wpattern;

	/* Copy ColorByNodeType configuration */
	to->by_nodetype = from->by_nodetype; /* struct assign */

	/* Copy ColorByTime configuration */
	to->by_timestamp = from->by_timestamp; /* struct assign */

	/* Copy ColorByWPattern configuration */
	to->by_wpattern = from->by_wpattern; /* struct assign */
	to->by_wpattern.wpgroup_list = NULL;
	wpgroup_llink = from->by_wpattern.wpgroup_list;
	while (wpgroup_llink != NULL) {
		wpgroup = (struct WPatternGroup *)wpgroup_llink->data;
		new_wpgroup = NEW(struct WPatternGroup);
		*new_wpgroup = *wpgroup; /* struct assign */
		G_LIST_APPEND(to->by_wpattern.wpgroup_list, new_wpgroup);

		new_wpgroup->wp_list = NULL;
		wp_llink = wpgroup->wp_list;
		while (wp_llink != NULL) {
			wpattern = (char *)wp_llink->data;
			G_LIST_APPEND(new_wpgroup->wp_list, xstrdup( wpattern ));
			wp_llink = wp_llink->next;
		}

		wpgroup_llink = wpgroup_llink->next;
	}
}


/* Destructor for a ColorConfig structure. This frees everything except
 * the main structure itself (so that this can be used to flush out a
 * statically allocated struct) */
void
color_config_destroy( struct ColorConfig *ccfg )
{
	struct WPatternGroup *wpgroup;
	GList *wpgroup_llink, *wp_llink;
	char *wpattern;

	wpgroup_llink = ccfg->by_wpattern.wpgroup_list;
	while (wpgroup_llink != NULL) {
		wpgroup = (struct WPatternGroup *)wpgroup_llink->data;

		wp_llink = wpgroup->wp_list;
		while (wp_llink != NULL) {
			wpattern = (char *)wp_llink->data;
			xfree( wpattern );
			wp_llink = wp_llink->next;
		}
		g_list_free( wpgroup->wp_list );

		xfree( wpgroup );
		wpgroup_llink = wpgroup_llink->next;
	}
	g_list_free( ccfg->by_wpattern.wpgroup_list );
}


ColorMode
color_get_mode( void )
{
	return color_mode;
}


/* fsn-mode Task A2: see color.h's doc comment -- a cheap peek at the
 * live spectrum type for ui_legend_draw( ) to poll every frame. */
SpectrumType
color_timestamp_spectrum_type( void )
{
	return color_config.by_timestamp.spectrum_type;
}


/* Returns (a copy of) the current color configuration. Note: It is the
 * responsibility of the caller to call color_config_destroy( ) on the
 * returned copy when it is no longer needed */
void
color_get_config( struct ColorConfig *ccfg )
{
	color_config_copy( ccfg, &color_config );
}


/* Returns the appropriate color for the given node, as per its type */
static const RGBcolor *
node_type_color( GNode *node )
{
	return &color_config.by_nodetype.colors[NODE_DESC(node)->type];
}


/* fsn-mode Task A2: returns the color for a file whose chosen timestamp
 * is `age_seconds` in the past, stepping through fsn_age_buckets[]
 * (src/fsn-style.h) in order and returning the first bucket whose
 * max_age_s the age does not exceed; the last bucket ("> 1 yr") is the
 * catch-all for anything older than the second-to-last cutoff. */
static const RGBcolor *
fsn_bucket_color( double age_seconds )
{
	static RGBcolor bucket_colors[FSN_AGE_BUCKET_COUNT];
	static boolean initialized = FALSE;
	int i;

	if (!initialized) {
		for (i = 0; i < FSN_AGE_BUCKET_COUNT; i++) {
			bucket_colors[i].r = fsn_age_buckets[i].rgb[0];
			bucket_colors[i].g = fsn_age_buckets[i].rgb[1];
			bucket_colors[i].b = fsn_age_buckets[i].rgb[2];
		}
		initialized = TRUE;
	}

	for (i = 0; i < FSN_AGE_BUCKET_COUNT - 1; i++)
		if (age_seconds <= fsn_age_buckets[i].max_age_s)
			return &bucket_colors[i];

	return &bucket_colors[FSN_AGE_BUCKET_COUNT - 1];
}


/* Returns the appropriate color for the given node, as per its timestamp */
static const RGBcolor *
time_color( GNode *node )
{
	double x;
        time_t node_time;
	int i;

	/* Directory override */
	if (NODE_IS_DIR(node))
		return node_type_color( node );

	/* Choose appropriate timestamp */
	switch (color_config.by_timestamp.timestamp_type) {
		case TIMESTAMP_ACCESS:
		node_time = NODE_DESC(node)->atime;
		break;

		case TIMESTAMP_MODIFY:
		node_time = NODE_DESC(node)->mtime;
		break;

		case TIMESTAMP_ATTRIB:
		node_time = NODE_DESC(node)->ctime;
		break;

		SWITCH_FAIL
	}

	/* fsn-mode Task A2: fsn's bucket ages are fixed, absolute cutoffs
	 * from *now* (7d/14d/30d/91d/182d/365d), exactly what the original
	 * fsn's "ages:" legend bar showed -- not the user-adjustable
	 * oldest/newest window the continuous rainbow/heat/gradient
	 * spectrums below use. Deliberately ignores
	 * color_config.by_timestamp.old_time/new_time for that reason. */
	if (color_config.by_timestamp.spectrum_type == SPECTRUM_FSN_BUCKETS)
		return fsn_bucket_color( difftime( time( NULL ), node_time ) );

	/* Temporal position value (0 = old, 1 = new) */
	x = difftime( node_time, color_config.by_timestamp.old_time ) / difftime( color_config.by_timestamp.new_time, color_config.by_timestamp.old_time );

	if (x < 0.0) {
		/* Node is off the spectrum (too old) */
		return &spectrum_underflow_color;
	}

	if (x > 1.0) {
		/* Node is off the spectrum (too new) */
		return &spectrum_overflow_color;
	}

	/* Return a color somewhere in the spectrum */
	i = (int)floor( x * (double)(SPECTRUM_NUM_SHADES - 1) );
	return &spectrum_colors[i];
}


/* Returns the appropriate color for the given node, as matched (or not
 * matched) to the current set of wildcard patterns */
static const RGBcolor *
wpattern_color( GNode *node )
{
	struct WPatternGroup *wpgroup;
	GList *wpgroup_llink, *wp_llink;
	const char *name, *wpattern;

	/* Directory override */
	if (NODE_IS_DIR(node))
		return node_type_color( node );

	name = NODE_DESC(node)->name;

	/* Search for a match in the wildcard pattern groups */
	wpgroup_llink = color_config.by_wpattern.wpgroup_list;
	while (wpgroup_llink != NULL) {
		wpgroup = (struct WPatternGroup *)wpgroup_llink->data;

		/* Check against patterns in this group */
		wp_llink = wpgroup->wp_list;
		while (wp_llink != NULL) {
			wpattern = (char *)wp_llink->data;
			if (!fnmatch( wpattern, name, FNM_FILE_NAME | FNM_PERIOD ))
				return &wpgroup->color; /* A match! */
			wp_llink = wp_llink->next;
		}

		wpgroup_llink = wpgroup_llink->next;
	}

	/* No match */
	return &color_config.by_wpattern.default_color;
}


/* (Re)assigns colors to all nodes rooted at the given node */
void
color_assign_recursive( GNode *dnode )
{
	GNode *node;
	const RGBcolor *color;

	g_assert( NODE_IS_DIR(dnode) || NODE_IS_METANODE(dnode) );

	geometry_queue_rebuild( dnode );

	node = dnode->children;
	while (node != NULL) {
		switch (color_mode) {
			case COLOR_BY_NODETYPE:
			color = node_type_color( node );
			break;

			case COLOR_BY_TIMESTAMP:
			color = time_color( node );
			break;

			case COLOR_BY_WPATTERN:
			color = wpattern_color( node );
			break;

			SWITCH_FAIL
		}
                NODE_DESC(node)->color = color;

		if (NODE_IS_DIR(node))
			color_assign_recursive( node );

		node = node->next;
	}
}


/* Changes the current color mode */
void
color_set_mode( ColorMode mode )
{
	color_mode = mode;
	color_assign_recursive( globals.fstree );
	redraw( );
}


/* Returns a color in the given type of spectrum, at the given position
 * x = [0, 1]. If the spectrum is of a type which requires parameters,
 * those are passed in via the data argument */
RGBcolor
color_spectrum_color( SpectrumType type, double x, void *data )
{
	RGBcolor color;
	RGBcolor *zero_color, *one_color;

	g_assert( (x >= 0.0) && (x <= 1.0) );

	switch (type) {
		case SPECTRUM_RAINBOW:
		return rainbow_color( 1.0 - x );

		case SPECTRUM_HEAT:
		return heat_color( x );

		case SPECTRUM_GRADIENT:
		zero_color = ((RGBcolor **)data)[0];
		one_color = ((RGBcolor **)data)[1];
		color.r = zero_color->r + x * (one_color->r - zero_color->r);
		color.g = zero_color->g + x * (one_color->g - zero_color->g);
		color.b = zero_color->b + x * (one_color->b - zero_color->b);
		return color;

		case SPECTRUM_FSN_BUCKETS:
		/* This function's x=[0,1] continuous-position contract has no
		 * real meaning for fsn's buckets (absolute ages, not a
		 * windowed spectrum -- see fsn_bucket_color( ) above); this
		 * case exists so the two callers that unconditionally sample
		 * color_spectrum_color( ) across x -- generate_spectrum_colors( )'s
		 * SPECTRUM_NUM_SHADES table and src/sdl/ui_dialogs.cpp's Color
		 * Setup preview strip -- get a real color instead of hitting
		 * SWITCH_FAIL, by stepping through the 7 bucket colors in
		 * order as x increases. The preview strip this actually
		 * drives ends up showing exactly the 7 bucket colors in
		 * order, which is a reasonable enough substitute for "preview
		 * of what this spectrum choice looks like". */
		{
			int i = (int)(x * (double)FSN_AGE_BUCKET_COUNT);
			if (i >= FSN_AGE_BUCKET_COUNT)
				i = FSN_AGE_BUCKET_COUNT - 1;
			color.r = fsn_age_buckets[i].rgb[0];
			color.g = fsn_age_buckets[i].rgb[1];
			color.b = fsn_age_buckets[i].rgb[2];
		}
		return color;

		SWITCH_FAIL
	}

	/* cc: duh... shouldn't there be a return value here? */
	color.r = -1.0; color.g = -1.0; color.b = -1.0; return color;
}


/* This sets up the spectrum color array */
static void
generate_spectrum_colors( void )
{
	RGBcolor *boundary_colors[2];
        double x;
	int i;
	void *data = NULL;

	if (color_config.by_timestamp.spectrum_type == SPECTRUM_GRADIENT) {
		boundary_colors[0] = &color_config.by_timestamp.old_color;
		boundary_colors[1] = &color_config.by_timestamp.new_color;
		data = boundary_colors;
	}

	for (i = 0; i < SPECTRUM_NUM_SHADES; i++) {
		x = (double)i / (double)(SPECTRUM_NUM_SHADES - 1);
		spectrum_colors[i] = color_spectrum_color( color_config.by_timestamp.spectrum_type, x, data ); /* struct assign */
	}

        /* Off-spectrum colors - make them dark */

	spectrum_underflow_color = spectrum_colors[0]; /* struct assign */
	spectrum_underflow_color.r *= 0.5;
	spectrum_underflow_color.g *= 0.5;
	spectrum_underflow_color.b *= 0.5;

	spectrum_overflow_color = spectrum_colors[(SPECTRUM_NUM_SHADES - 1)]; /* struct assign */
	spectrum_overflow_color.r *= 0.5;
	spectrum_overflow_color.g *= 0.5;
	spectrum_overflow_color.b *= 0.5;
}


/* Changes the current color configuration, and if mode is not COLOR_NONE,
 * sets the color mode as well */
void
color_set_config( struct ColorConfig *new_ccfg, ColorMode mode )
{
	color_config_destroy( &color_config );
	color_config_copy( &color_config, new_ccfg );

	generate_spectrum_colors( );

	if (globals.fsv_mode == FSV_SPLASH) {
		g_assert( mode != COLOR_NONE );
		color_mode = mode;
	}
	else if (mode != COLOR_NONE)
		color_set_mode( mode );
	else
		color_set_mode( color_mode );
}


/* Reads color configuration from file */
static void
color_read_config( void )
{
	struct WPatternGroup *wpgroup;
	NVStore *fsvrc;
	int i, x;
	char *str;

	fsvrc = nvs_open( CONFIG_FILE );

	nvs_change_path( fsvrc, key_color );

	/* Color mode. Was reading back "mode" while color_write_config()
	 * below writes key_color_mode ("colormode") -- a pre-existing typo
	 * that nvstore.c's now-fixed (Task 5.3) all-stub implementation had
	 * silently masked forever, since every *_default() read always just
	 * returned its default regardless of key. Fixed to read back the
	 * same key it's written under, so the color mode actually survives
	 * a relaunch. */
	x = nvs_read_int_token_default( fsvrc, key_color_mode, tokens_color_mode, default_color_mode );
	color_mode = (ColorMode)x;

	/* ColorByNodeType configuration */
	nvs_change_path( fsvrc, key_nodetype );
	for (i = 1; i < NUM_NODE_TYPES; i++) {
		str = nvs_read_string_default( fsvrc, keys_nodetype_node_type[i], default_nodetype_colors[i] );
		color_config.by_nodetype.colors[i] = hex2rgb( str ); /* struct assign */
		free( str ); /* !xfree */
	}
	nvs_change_path( fsvrc, ".." );

	/* ColorByTime configuration */
	nvs_change_path( fsvrc, key_timestamp );
	x = nvs_read_int_token_default( fsvrc, key_timestamp_spectrum_type, tokens_timestamp_spectrum_type, default_timestamp_spectrum_type );
	color_config.by_timestamp.spectrum_type = (SpectrumType)x;
	x = nvs_read_int_token_default( fsvrc, key_timestamp_timestamp_type, tokens_timestamp_timestamp_type, default_timestamp_timestamp_type );
	color_config.by_timestamp.timestamp_type = (TimeStampType)x;
	x = nvs_read_int_default( fsvrc, key_timestamp_period, default_timestamp_period );
	color_config.by_timestamp.new_time = time( NULL );
	color_config.by_timestamp.old_time = color_config.by_timestamp.new_time - (time_t)x;
	str = nvs_read_string_default( fsvrc, key_timestamp_old_color, default_timestamp_old_color );
	color_config.by_timestamp.old_color = hex2rgb( str ); /* struct assign */
	free( str ); /* !xfree */
	str = nvs_read_string_default( fsvrc, key_timestamp_new_color, default_timestamp_new_color );
	color_config.by_timestamp.new_color = hex2rgb( str ); /* struct assign */
	free( str ); /* !xfree */
	nvs_change_path( fsvrc, ".." );

	/* ColorByWPattern configuration */
	nvs_change_path( fsvrc, key_wpattern );
	/* Wildcard pattern groups */
	color_config.by_wpattern.wpgroup_list = NULL;
	nvs_vector_begin( fsvrc );
	while (nvs_path_present( fsvrc, key_wpattern_group )) {
		nvs_change_path( fsvrc, key_wpattern_group );

		wpgroup = NEW(struct WPatternGroup);
		str = nvs_read_string( fsvrc, key_wpattern_group_color );
		wpgroup->color = hex2rgb( str );
		free( str ); /* !xfree */

		/* Read in patterns */
		wpgroup->wp_list = NULL;
		nvs_vector_begin( fsvrc );
		while (nvs_path_present( fsvrc, key_wpattern_group_wpattern )) {
			str = nvs_read_string( fsvrc, key_wpattern_group_wpattern );
			G_LIST_APPEND(wpgroup->wp_list, xstrdup( str ));
			free( str ); /* !xfree */
		}
		nvs_vector_end( fsvrc );

		G_LIST_APPEND(color_config.by_wpattern.wpgroup_list, wpgroup);

		nvs_change_path( fsvrc, ".." );
	}
	/* Close the "group" vector opened above -- mirrors
	 * color_write_config()'s own nvs_vector_end() before its
	 * defaultcolor write below. Missing this left the vector open with
	 * `current` still == the wpattern node: nvs_read_string_default()'s
	 * scalar_get() (nvstore.c) would then treat "defaultcolor" as if it
	 * were itself the N-th repeat of a vector key, requiring an N-th
	 * *sibling* named "defaultcolor" that doesn't exist (there is only
	 * ever one) -- returning "" -> hex2rgb("") -> black, for every
	 * unmatched file, on every load after at least one wildcard group
	 * had ever been saved. */
	nvs_vector_end( fsvrc );
	/* Default color */
	str = nvs_read_string_default( fsvrc, key_wpattern_default_color, default_wpattern_default_color );
	color_config.by_wpattern.default_color = hex2rgb( str ); /* struct assign */
	free( str ); /* !xfree */
	nvs_change_path( fsvrc, ".." );

	nvs_change_path( fsvrc, ".." );

	nvs_close( fsvrc );
}


/* Writes color configuration to file */
void
color_write_config( void )
{
	struct WPatternGroup *wpgroup;
	NVStore *fsvrc;
	GList *wpgroup_llink, *wp_llink;
	int i;
	char *wpattern;

	fsvrc = nvs_open( CONFIG_FILE );

	/* Clean out existing color configuration information */
	nvs_change_path( fsvrc, key_color );
	nvs_delete_recursive( fsvrc, "." );

	/* Color mode */
	nvs_write_int_token( fsvrc, key_color_mode, color_mode, tokens_color_mode );

	/* ColorByNodeType configuration */
	nvs_change_path( fsvrc, key_nodetype );
	for (i = 1; i < NUM_NODE_TYPES; i++)
		nvs_write_string( fsvrc, keys_nodetype_node_type[i], rgb2hex( &color_config.by_nodetype.colors[i] ) );
	nvs_change_path( fsvrc, ".." );

	/* ColorByTime configuration */
	nvs_change_path( fsvrc, key_timestamp );
	nvs_write_int_token( fsvrc, key_timestamp_spectrum_type, color_config.by_timestamp.spectrum_type, tokens_timestamp_spectrum_type );
	nvs_write_int_token( fsvrc, key_timestamp_timestamp_type, color_config.by_timestamp.timestamp_type, tokens_timestamp_timestamp_type );
	nvs_write_int( fsvrc, key_timestamp_period, (int)difftime( color_config.by_timestamp.new_time, color_config.by_timestamp.old_time ) );
	nvs_write_string( fsvrc, key_timestamp_old_color, rgb2hex( &color_config.by_timestamp.old_color ) );
	nvs_write_string( fsvrc, key_timestamp_new_color, rgb2hex( &color_config.by_timestamp.new_color ) );
	nvs_change_path( fsvrc, ".." );

	/* ColorByWPattern configuration */
	nvs_change_path( fsvrc, key_wpattern );
	nvs_vector_begin( fsvrc );
	wpgroup_llink = color_config.by_wpattern.wpgroup_list;
	while (wpgroup_llink != NULL) {
		wpgroup = (struct WPatternGroup *)wpgroup_llink->data;

		nvs_change_path( fsvrc, key_wpattern_group );
		nvs_write_string( fsvrc, key_wpattern_group_color, rgb2hex( &wpgroup->color ) );

		nvs_vector_begin( fsvrc );
		wp_llink = wpgroup->wp_list;
		while (wp_llink != NULL) {
			wpattern = (char *)wp_llink->data;
			nvs_write_string( fsvrc, key_wpattern_group_wpattern, wpattern );
			wp_llink = wp_llink->next;
		}
		nvs_vector_end( fsvrc );

		nvs_change_path( fsvrc, ".." );

		wpgroup_llink = wpgroup_llink->next;
	}
	nvs_vector_end( fsvrc );
	nvs_write_string( fsvrc, key_wpattern_default_color, rgb2hex( &color_config.by_wpattern.default_color ) );
	nvs_change_path( fsvrc, ".." );

	nvs_close( fsvrc );
}


/* First-time initialization */
void
color_init( void )
{
	/* Read configuration file */
	color_read_config( );

	/* Update radio menu in window with configured color mode */
	window_set_color_mode( color_mode );

	/* Generate spectrum color table */
	generate_spectrum_colors( );
}


/* Returns the current landscape preset (index into fsn_landscapes[],
 * src/fsn-style.h), for src/sdl/ui_main.cpp's Display->Landscape menu to
 * mark the active radio item -- same shape as color_get_mode( ) above. */
int
landscape_get( void )
{
	return landscape_current;
}


/* Shared by landscape_set( ) and landscape_set_auto( ) below: apply the
 * preset to the renderer, persist the raw "landscape" nvstore key, and
 * redraw. What differs between the two public entry points is only
 * whether the "landscape_explicit" flag is also touched -- see that
 * pair's own doc comments (color.h) for why the distinction exists. */
static void
landscape_apply( int index )
{
	NVStore *fsvrc;

	if (index < 0 || index >= FSN_LANDSCAPE_COUNT)
		index = default_landscape;

	landscape_current = index;
	gpu_set_landscape( index );

	fsvrc = nvs_open( CONFIG_FILE );
	nvs_write_int_token( fsvrc, key_landscape, index, tokens_landscape );
	nvs_close( fsvrc );

	redraw( );
}


/* Changes the current landscape preset, pushes it to the renderer, and
 * persists it immediately (nvstore key "landscape", by name -- see
 * tokens_landscape[] above), mirroring color_set_mode( )'s "change,
 * apply, save" shape rather than color_write_config( )'s separate
 * apply-then-save-on-demand one: a menu click is a single, immediate
 * action with no intervening dialog to Cancel out of.
 *
 * src/sdl/ui_main.cpp's Display -> Landscape menu is this function's one
 * and only caller, which is what makes it the right place to also mark
 * the choice "explicit" (fsn-mode Task B3, color.h's landscape_explicit( )
 * doc comment) -- landscape_set_auto( ) below is for every other caller. */
void
landscape_set( int index )
{
	NVStore *fsvrc;

	landscape_apply( index );

	landscape_explicit_current = TRUE;
	fsvrc = nvs_open( CONFIG_FILE );
	nvs_write_boolean( fsvrc, key_landscape_explicit, TRUE );
	nvs_close( fsvrc );
}


/* fsn-mode Task B3: FSN mode's own auto-default (src/sdl/main.cpp), a
 * separate entry point from landscape_set( ) purely so it can apply and
 * persist the very same way without also claiming to be the user's
 * explicit choice -- see color.h's doc comment on both functions. */
void
landscape_set_auto( int index )
{
	landscape_apply( index );
}


boolean
landscape_explicit( void )
{
	return landscape_explicit_current;
}


/* Reads the landscape preset from ~/.fsvrc (default: "slate", i.e.
 * today's pre-A1 look) and pushes it to the renderer. Does not call
 * redraw( ): this runs during startup, before the first frame, the same
 * way color_init( ) never calls it either. */
void
landscape_init( void )
{
	NVStore *fsvrc;
	int index;

	fsvrc = nvs_open( CONFIG_FILE );
	index = nvs_read_int_token_default( fsvrc, key_landscape, tokens_landscape, default_landscape );
	landscape_explicit_current = nvs_read_boolean_default( fsvrc, key_landscape_explicit, FALSE );
	nvs_close( fsvrc );

	landscape_current = index;
	gpu_set_landscape( index );
}


/* end color.c */
