/* tests/test_color_persistence.c
 *
 * SPDX-License-Identifier: MIT
 *
 * Regression test for the Critical-2 fix-round bug: src/color.c's
 * color_read_config() was missing the nvs_vector_end() that closes the
 * "group" vector before reading `defaultcolor` -- so once at least one
 * wildcard group had ever been saved, the by_wpattern default color
 * read back as "" -> hex2rgb("") -> BLACK on every subsequent load,
 * on both frontends (Task 5.3 headed verification happened not to
 * render this, since its last Apply switched the live mode to
 * COLOR_BY_TIMESTAMP before quitting).
 *
 * Exercises the *real* src/color.c entry points end to end -- not
 * nvstore.c directly (tests/test_nvstore.c's own
 * test_vector_must_be_closed_before_sibling_scalar() already covers
 * the underlying nvstore mechanism in isolation) -- linked against
 * libfsvcore the same way tests/test_scanfs.c is, with
 * tools/fsv-headless-stubs.c standing in for the GTK/SDL frontend
 * hooks color.c calls through (window_set_color_mode(), redraw(),
 * etc.). $HOME is redirected to a private temp directory before
 * anything touches CONFIG_FILE ("~/.fsvrc"), so this never reads or
 * writes the real invoking user's config.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "color.h"

int
main(void)
{
	struct ColorConfig cfg;
	struct ColorConfig reloaded;
	struct WPatternGroup *group;
	const RGBcolor kNonBlackDefault = { 0.0f, 1.0f, 1.0f }; /* cyan --
	    as far from "" -> hex2rgb("") -> {0,0,0} black as a color gets */
	const RGBcolor kGroupColor = { 0.0f, 1.0f, 0.0f }; /* green */

	/* Redirect CONFIG_FILE ("~/.fsvrc") to a private directory --
	 * before color_init() or anything else in color.c ever runs. */
	if (setenv("HOME", "/tmp/test_color_persistence_home", 1) != 0) {
		perror("setenv");
		return 1;
	}
	system("mkdir -p /tmp/test_color_persistence_home");
	system("rm -f /tmp/test_color_persistence_home/.fsvrc");

	/* First "session": no config file yet -- defaults apply. */
	color_init();
	assert(color_get_mode() == COLOR_BY_NODETYPE);

	/* Build a scratch config with exactly one wildcard group (matching
	 * "*.c") and a distinctly non-black default color -- the real
	 * shape color_write_config()'s "group" vector writes, and the
	 * exact shape that triggers the bug on the read side once at least
	 * one group exists. */
	color_get_config(&cfg);

	group = NEW(struct WPatternGroup);
	group->color = kGroupColor;
	group->wp_list = NULL;
	G_LIST_APPEND(group->wp_list, xstrdup("*.c"));
	cfg.by_wpattern.wpgroup_list = NULL;
	G_LIST_APPEND(cfg.by_wpattern.wpgroup_list, group);
	cfg.by_wpattern.default_color = kNonBlackDefault;

	/* color_set_config()'s mode != COLOR_NONE branch calls
	 * color_set_mode() -> color_assign_recursive(globals.fstree) --
	 * this test never scanfs()'d anything, so globals.fstree is NULL.
	 * FSV_SPLASH is the one globals.fsv_mode value color_set_config()
	 * itself special-cases to skip that tree walk entirely (just
	 * "color_mode = mode;") -- exactly fsv.c's own real splash-screen
	 * use of this function, before any filesystem has been scanned. */
	globals.fsv_mode = FSV_SPLASH;

	/* Port of ui_dialogs.cpp's "Apply" button (and, before it,
	 * dialog.c's csdialog_ok_button_cb() + the write this task added to
	 * it): commit to the live config, switch mode, persist to disk. */
	color_set_config(&cfg, COLOR_BY_WPATTERN);
	color_write_config();
	color_config_destroy(&cfg);

	assert(color_get_mode() == COLOR_BY_WPATTERN);

	/* Simulate a relaunch: re-read from the file color_write_config()
	 * above just wrote. color_read_config() itself is file-static (not
	 * exported), so color_init() -- its only caller -- is the way in;
	 * calling it twice in one process re-reads unconditionally (every
	 * field in the file gets its real value or its hardcoded default,
	 * never "whatever it already was" -- see color_read_config()'s own
	 * body), so this is a faithful stand-in for a fresh process's first
	 * color_init() call. */
	color_init();

	assert(color_get_mode() == COLOR_BY_WPATTERN);

	color_get_config(&reloaded);

	/* THE regression assertion: before the fix, this was {0,0,0}
	 * (hex2rgb("")), not the cyan actually saved. */
	assert(reloaded.by_wpattern.default_color.r == kNonBlackDefault.r);
	assert(reloaded.by_wpattern.default_color.g == kNonBlackDefault.g);
	assert(reloaded.by_wpattern.default_color.b == kNonBlackDefault.b);
	assert(!(reloaded.by_wpattern.default_color.r == 0.0f &&
	         reloaded.by_wpattern.default_color.g == 0.0f &&
	         reloaded.by_wpattern.default_color.b == 0.0f));

	/* The group itself round-tripped too (the read side's vector
	 * handling for "group"/"wp" was never the bug -- only what came
	 * right after it). */
	assert(reloaded.by_wpattern.wpgroup_list != NULL);
	{
		struct WPatternGroup *g =
		    (struct WPatternGroup *)reloaded.by_wpattern.wpgroup_list->data;
		assert(g->color.r == kGroupColor.r);
		assert(g->color.g == kGroupColor.g);
		assert(g->color.b == kGroupColor.b);
		assert(g->wp_list != NULL);
		assert(!strcmp((const char *)g->wp_list->data, "*.c"));
		assert(reloaded.by_wpattern.wpgroup_list->next == NULL); /* exactly one group */
	}

	color_config_destroy(&reloaded);

	/* fsn-mode Task A2: SPECTRUM_FSN_BUCKETS round-trips through
	 * ~/.fsvrc too -- same "set, write, reload, assert" shape as the
	 * by_wpattern regression above, but exercising color.c's
	 * tokens_timestamp_spectrum_type[] (a STRING-token nvstore array,
	 * so the new enumerator's *position* in that array -- not its
	 * numeric value in SpectrumType -- is what actually gets persisted;
	 * see color.h's own doc comment on SPECTRUM_FSN_BUCKETS). Reuses
	 * `cfg`/`reloaded` (both already destroyed above) as fresh scratch
	 * configs rather than declaring new ones. */
	color_get_config(&cfg);
	cfg.by_timestamp.spectrum_type = SPECTRUM_FSN_BUCKETS;
	color_set_config(&cfg, COLOR_BY_TIMESTAMP);
	color_write_config();
	color_config_destroy(&cfg);

	assert(color_get_mode() == COLOR_BY_TIMESTAMP);

	color_init(); /* simulated relaunch, same as above */

	assert(color_get_mode() == COLOR_BY_TIMESTAMP);

	color_get_config(&reloaded);
	assert(reloaded.by_timestamp.spectrum_type == SPECTRUM_FSN_BUCKETS);
	color_config_destroy(&reloaded);

	fprintf(stderr, "test_color_persistence: all OK\n");
	return 0;
}
