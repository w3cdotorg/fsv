/* tests/test_nvstore.c
 *
 * SPDX-License-Identifier: MIT
 *
 * Unit tests for lib/nvstore.c -- added alongside the fix round that
 * turned it from a complete stub into real logic (Task 5.3 of the
 * SDL/Metal port; see docs/PORTING.md). Covers:
 *
 *   1. A fixture round-trip shaped exactly like src/color.c's real
 *      usage (nested groups, two levels of vectors) -- write, dump,
 *      read back, missing-file defaults, backslash/tab/newline
 *      escaping.
 *   2. The exact bug class Critical 1 of the fix round fixed: a
 *      corrupt on-disk file (first line indented, or a depth jump of
 *      more than one level) must fall back to defaults, never crash --
 *      previously an uninitialized-stack read.
 *   3. The exact bug class Critical 2 fixed: reading a plain scalar key
 *      at a node while a vector opened on that *same* node is still
 *      open (a missing nvs_vector_end()) misreads it as a vector slot.
 *      Demonstrated directly against nvstore's own API, independent of
 *      color.c's specific fix, so a future caller that makes the same
 *      mistake fails a test rather than shipping silently.
 *   4. nvs_read_int_token_default()'s fallback to default_val for a
 *      present-but-unrecognized token value (Minor 4), not tokens[0].
 *   5. A locale-independent float round-trip (Important 3): writes and
 *      reads back under whatever locale is available, and additionally
 *      under a comma-decimal locale (fr_FR) if the platform has it
 *      installed -- skipped, not failed, if not (this environment's
 *      Linux CI container has no locales generated beyond C/POSIX).
 */

#include <assert.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nvstore.h"

static const char *mode_tokens[] = { "nodetype", "time", "wpattern", NULL };

/* ---- 1. Fixture round-trip, shaped like color.c's real usage ---- */

static void
test_fixture_roundtrip(void)
{
	const char *path = "/tmp/test_nvstore_fixture.rc";
	NVStore *nvs;
	char *s;
	int ngroups;

	unlink(path);

	nvs = nvs_open(path);
	assert(nvs != NULL);
	nvs_change_path(nvs, "color");
	nvs_delete_recursive(nvs, ".");

	nvs_write_int_token(nvs, "colormode", 2, mode_tokens); /* wpattern */

	nvs_change_path(nvs, "nodetype");
	nvs_write_string(nvs, "directory", "#A0A0A0");
	nvs_write_string(nvs, "regularfile", "#FFFFA0");
	nvs_change_path(nvs, "..");

	nvs_change_path(nvs, "wpattern");
	nvs_vector_begin(nvs);
	{
		nvs_change_path(nvs, "group");
		nvs_write_string(nvs, "color", "#FF0000");
		nvs_vector_begin(nvs);
		nvs_write_string(nvs, "wp", "*.c");
		nvs_write_string(nvs, "wp", "*.h");
		nvs_vector_end(nvs);
		nvs_change_path(nvs, "..");

		nvs_change_path(nvs, "group");
		nvs_write_string(nvs, "color", "#0000FF");
		nvs_vector_begin(nvs);
		nvs_write_string(nvs, "wp", "*.txt");
		nvs_vector_end(nvs);
		nvs_change_path(nvs, "..");
	}
	nvs_vector_end(nvs); /* Critical 2's fix, exercised: this MUST run
	                      * before the defaultcolor write below, or the
	                      * write would misbehave the same way the read
	                      * side's missing call did. */
	nvs_write_string(nvs, "defaultcolor", "#FFFFA0");
	nvs_change_path(nvs, "..");

	nvs_change_path(nvs, "..");
	assert(nvs_close(nvs));

	/* Read back */
	nvs = nvs_open(path);
	assert(nvs != NULL);
	nvs_change_path(nvs, "color");

	assert(nvs_read_int_token_default(nvs, "colormode", mode_tokens, 0) == 2);

	nvs_change_path(nvs, "nodetype");
	s = nvs_read_string_default(nvs, "directory", "MISSING");
	assert(!strcmp(s, "#A0A0A0"));
	free(s);
	nvs_change_path(nvs, "..");

	nvs_change_path(nvs, "wpattern");
	ngroups = 0;
	nvs_vector_begin(nvs);
	while (nvs_path_present(nvs, "group")) {
		char *color;
		int npatterns = 0;

		nvs_change_path(nvs, "group");
		color = nvs_read_string(nvs, "color");

		nvs_vector_begin(nvs);
		while (nvs_path_present(nvs, "wp")) {
			char *wp = nvs_read_string(nvs, "wp");
			if (ngroups == 0 && npatterns == 0)
				assert(!strcmp(wp, "*.c"));
			free(wp);
			npatterns++;
		}
		nvs_vector_end(nvs);

		if (ngroups == 0) {
			assert(!strcmp(color, "#FF0000"));
			assert(npatterns == 2);
		} else if (ngroups == 1) {
			assert(!strcmp(color, "#0000FF"));
			assert(npatterns == 1);
		}
		free(color);
		nvs_change_path(nvs, "..");
		ngroups++;
	}
	nvs_vector_end(nvs); /* the exact call Critical 2 was missing */
	assert(ngroups == 2);

	/* This is the Critical-2 regression: reading a plain scalar key at
	 * this same node, right after closing the vector that iterated
	 * "group" here, must return the real value -- not "" (misread as
	 * vector slot N of a key with only one instance). */
	s = nvs_read_string_default(nvs, "defaultcolor", "MISSING");
	assert(!strcmp(s, "#FFFFA0"));
	free(s);

	nvs_change_path(nvs, "..");
	nvs_change_path(nvs, "..");
	nvs_close(nvs);
	unlink(path);
}

/* ---- 1b. Missing file: every *_default() must fall back cleanly ---- */

static void
test_missing_file_defaults(void)
{
	const char *path = "/tmp/test_nvstore_missing.rc";
	NVStore *nvs;
	char *s;

	unlink(path);
	nvs = nvs_open(path);
	assert(nvs != NULL);
	nvs_change_path(nvs, "color");
	assert(nvs_read_int_token_default(nvs, "colormode", mode_tokens, 0) == 0);
	s = nvs_read_string_default(nvs, "nope", "fallback");
	assert(!strcmp(s, "fallback"));
	free(s);
	nvs_close(nvs);
}

/* ---- 1c. Escaping ---- */

static void
test_escaping(void)
{
	const char *path = "/tmp/test_nvstore_escape.rc";
	NVStore *nvs;
	char *v;

	unlink(path);
	nvs = nvs_open(path);
	nvs_change_path(nvs, "x");
	nvs_write_string(nvs, "tricky", "a\\b\tc\nd");
	nvs_change_path(nvs, "..");
	nvs_close(nvs);

	nvs = nvs_open(path);
	nvs_change_path(nvs, "x");
	v = nvs_read_string(nvs, "tricky");
	assert(!strcmp(v, "a\\b\tc\nd"));
	free(v);
	nvs_close(nvs);
	unlink(path);
}

/* ---- 1d. Atomic save (Critical 1's second half) ----
 *
 * save_file() writes to "<path>.tmp" then rename()s it over the real
 * path -- a single directory-entry swap, atomic on every POSIX
 * filesystem this project targets (same-directory rename of a regular
 * file). A kill-signal race test (start a large write, SIGKILL mid-
 * write, assert the original survives) would exercise the same
 * guarantee, but non-deterministically -- the window between fopen()
 * and fclose() is a handful of buffered fwrite() calls, not something
 * a test can reliably land a signal inside without either flaking or
 * padding the write out artificially (which would then be testing the
 * padding, not this code). The two things that ARE deterministic and
 * worth asserting directly: (a) a successful save leaves no leftover
 * "<path>.tmp" behind (proving the rename actually ran, not just that
 * *a* file got written somewhere), and (b) the file at the *real* path
 * only ever contains one fully-formed generation's content, never a
 * half-written one -- which follows from nvstore never opening the
 * real path for writing at all: every write targets ".tmp" exclusively,
 * so the real path is untouched until rename() replaces it in one
 * step. (The two Critical-1 corrupt-file tests above are what a failed
 * atomicity guarantee would actually look like in practice -- a
 * process killed after Critical 1's *original*, non-atomic
 * fopen(path,"w") would have left exactly the "some lines written,
 * then EOF" shape those tests construct by hand.) */
static void
test_atomic_save_leaves_no_tmp_file(void)
{
	const char *path = "/tmp/test_nvstore_atomic.rc";
	const char *tmp_path = "/tmp/test_nvstore_atomic.rc.tmp";
	NVStore *nvs;
	FILE *f;
	char *v;

	unlink(path);
	unlink(tmp_path);

	nvs = nvs_open(path);
	nvs_change_path(nvs, "k");
	nvs_write_string(nvs, "v", "first-generation");
	nvs_change_path(nvs, "..");
	assert(nvs_close(nvs));

	f = fopen(tmp_path, "r");
	assert(f == NULL); /* rename() consumed it -- nothing left at .tmp */

	/* A second save (a later color_write_config() call, in practice)
	 * fully replaces the first generation's content -- not appends to
	 * or corrupts it. */
	nvs = nvs_open(path);
	nvs_change_path(nvs, "k");
	nvs_write_string(nvs, "v", "second-generation");
	nvs_change_path(nvs, "..");
	assert(nvs_close(nvs));

	f = fopen(tmp_path, "r");
	assert(f == NULL);

	nvs = nvs_open(path);
	nvs_change_path(nvs, "k");
	v = nvs_read_string(nvs, "v");
	assert(!strcmp(v, "second-generation"));
	free(v);
	nvs_close(nvs);

	unlink(path);
}

/* ---- 2. Corrupt-file handling (Critical 1) ---- */

/* Writes `contents` verbatim to `path` (bypassing nvstore's own writer
 * entirely -- these are hand-crafted malformed files, not anything
 * nvs_close() would ever produce). */
static void
write_raw(const char *path, const char *contents)
{
	FILE *f = fopen(path, "w");
	assert(f != NULL);
	fputs(contents, f);
	fclose(f);
}

static void
test_corrupt_first_line_indented(void)
{
	const char *path = "/tmp/test_nvstore_corrupt_indent.rc";
	NVStore *nvs;
	char *s;

	/* First line starts with a tab -- depth 1 with no depth-0 parent
	 * ever seen. Previously read stack_at_depth[0] uninitialized. */
	write_raw(path, "\tfoo bar\n\tbaz qux\n");

	nvs = nvs_open(path);
	assert(nvs != NULL); /* must not crash */
	nvs_change_path(nvs, "color");
	s = nvs_read_string_default(nvs, "colormode", "DEFAULT");
	assert(!strcmp(s, "DEFAULT")); /* fell back cleanly, nothing parsed */
	free(s);
	nvs_close(nvs);
	unlink(path);
}

static void
test_corrupt_depth_jump(void)
{
	const char *path = "/tmp/test_nvstore_corrupt_jump.rc";
	NVStore *nvs;
	char *s;

	/* "a" at depth 0, then straight to depth 2 -- stack_at_depth[1] was
	 * never populated. */
	write_raw(path, "a\n\t\tb value\n");

	nvs = nvs_open(path);
	assert(nvs != NULL);
	nvs_change_path(nvs, "a");
	/* The malformed line aborted the whole parse (see load_file()'s own
	 * doc comment on why a skip-only recovery isn't safe here), so even
	 * the well-formed "a" line from before the bad one is gone --
	 * everything reads back as default. */
	s = nvs_read_string_default(nvs, "b", "DEFAULT");
	assert(!strcmp(s, "DEFAULT"));
	free(s);
	nvs_close(nvs);
	unlink(path);
}

static void
test_corrupt_stale_slot_reuse(void)
{
	const char *path = "/tmp/test_nvstore_corrupt_stale.rc";
	NVStore *nvs;
	char *s;

	/* "a" (depth0) -> "b" (depth1) -> "c" (depth2) -> "a2" (depth0,
	 * ends the "a" subtree) -> "d" (depth2). Without load_file()
	 * clearing stack_at_depth[1..] the moment "a2" is seen, "d"'s
	 * parent would silently resolve to the *old* "b" (a stale, non-NULL
	 * pointer left over from the "a" subtree) instead of being caught
	 * at all -- mis-nesting "d" under a subtree "a2" never actually
	 * opened. With that clearing in place, stack_at_depth[1] is NULL by
	 * the time "d" is parsed, so this hits the same "populated parent
	 * slot" check and abandon-the-load path as the other two corrupt-
	 * file cases -- proving the clearing step is what makes that check
	 * effective against a *stale*, not just an *uninitialized*, slot. */
	write_raw(path, "a\n\tb\n\t\tc val1\na2\n\t\td val2\n");

	nvs = nvs_open(path);
	assert(nvs != NULL);
	nvs_change_path(nvs, "a2");
	s = nvs_read_string_default(nvs, "d", "DEFAULT");
	assert(!strcmp(s, "DEFAULT"));
	free(s);
	nvs_close(nvs);
	unlink(path);
}

/* ---- 3. Vector/scalar interaction at the same node (Critical 2,
 * exercised directly against nvstore.h rather than through color.c) ---- */

static void
test_vector_must_be_closed_before_sibling_scalar(void)
{
	const char *path = "/tmp/test_nvstore_vector_scope.rc";
	NVStore *nvs;
	char *s;

	unlink(path);
	nvs = nvs_open(path);
	nvs_change_path(nvs, "n");
	nvs_vector_begin(nvs); /* two real "rep" siblings -- without this,
	                        * scalar_set()'s non-vector branch would
	                        * just overwrite the same node twice */
	nvs_write_string(nvs, "rep", "one");
	nvs_write_string(nvs, "rep", "two");
	nvs_vector_end(nvs);
	nvs_write_string(nvs, "solo", "value");
	nvs_change_path(nvs, "..");
	nvs_close(nvs);

	nvs = nvs_open(path);
	nvs_change_path(nvs, "n");
	nvs_vector_begin(nvs);
	while (nvs_path_present(nvs, "rep")) {
		char *v = nvs_read_string(nvs, "rep");
		free(v);
	}
	/* Deliberately NOT calling nvs_vector_end() here first, to prove
	 * the failure mode Critical 2 was. Not "MISSING" (the *_default()
	 * caller's own fallback): nvs_read_string_default()'s presence
	 * check (scalar_present(), vector-blind) sees "solo" genuinely
	 * exists and skips its own fallback, then delegates to
	 * nvs_read_string() -- vector-*aware* -- which misreads "solo" as
	 * the vector's current index into a key that only has one
	 * instance, finds nothing, and returns "" (nvs_read_string()'s own
	 * "not found" value, never NULL). This exact empty-string result --
	 * not a clean fallback to a caller-supplied default -- is what
	 * color_read_config() actually got back for `defaultcolor`
	 * before Critical 2 was fixed, which is why it decoded as black
	 * (hex2rgb("")) rather than some more obviously-wrong value. */
	s = nvs_read_string_default(nvs, "solo", "MISSING");
	assert(!strcmp(s, ""));
	free(s);
	nvs_close(nvs);

	/* Now the correct usage: */
	nvs = nvs_open(path);
	nvs_change_path(nvs, "n");
	nvs_vector_begin(nvs);
	while (nvs_path_present(nvs, "rep")) {
		char *v = nvs_read_string(nvs, "rep");
		free(v);
	}
	nvs_vector_end(nvs);
	s = nvs_read_string_default(nvs, "solo", "MISSING");
	assert(!strcmp(s, "value"));
	free(s);
	nvs_close(nvs);
	unlink(path);
}

/* ---- 4. Unrecognized token falls back to default_val, not tokens[0] ---- */

static void
test_unrecognized_token_falls_back(void)
{
	const char *path = "/tmp/test_nvstore_badtoken.rc";
	NVStore *nvs;
	int mode;

	unlink(path);
	nvs = nvs_open(path);
	nvs_change_path(nvs, "color");
	nvs_write_string(nvs, "colormode", "not_a_real_token");
	nvs_change_path(nvs, "..");
	nvs_close(nvs);

	nvs = nvs_open(path);
	nvs_change_path(nvs, "color");
	mode = nvs_read_int_token_default(nvs, "colormode", mode_tokens, 2);
	/* Must be the caller's default_val (2), not tokens[0]'s index (0) --
	 * the exact aliasing Minor 4 fixed. */
	assert(mode == 2);
	nvs_close(nvs);
	unlink(path);
}

/* ---- 5. Locale-independent float round-trip ---- */

static void
roundtrip_float_under_current_locale(double val)
{
	const char *path = "/tmp/test_nvstore_float.rc";
	NVStore *nvs;
	double back;

	unlink(path);
	nvs = nvs_open(path);
	nvs_change_path(nvs, "f");
	nvs_write_float(nvs, "v", val);
	nvs_change_path(nvs, "..");
	nvs_close(nvs);

	nvs = nvs_open(path);
	nvs_change_path(nvs, "f");
	back = nvs_read_float(nvs, "v");
	nvs_close(nvs);
	unlink(path);

	assert(fabs(back - val) < 1e-9);
}

static void
test_float_locale_roundtrip(void)
{
	const double val = 1234.5678;

	/* Whatever the ambient locale is (normally C/POSIX for a test
	 * binary) -- this alone would already have caught the old
	 * snprintf("%.17g")/atof() bug on a system where the *default*
	 * locale has a comma decimal point. */
	roundtrip_float_under_current_locale(val);

	/* The actual bug scenario: write under a comma-decimal locale,
	 * read back under the default (or vice versa) -- either order
	 * exercises the write-side and read-side translation independently.
	 * Skipped, not failed, if fr_FR isn't installed (true of this
	 * project's minimal Linux CI container; true of most macOS/BSD
	 * installs by default too unless explicitly added). */
	if (setlocale(LC_NUMERIC, "fr_FR.UTF-8") == NULL &&
	    setlocale(LC_NUMERIC, "fr_FR") == NULL) {
		fprintf(stderr,
		    "test_nvstore: fr_FR locale not installed -- skipping "
		    "comma-decimal round-trip (still ran under the ambient "
		    "locale above)\n");
		return;
	}

	{
		const char *path = "/tmp/test_nvstore_float_locale.rc";
		NVStore *nvs;
		double back;

		/* Confirm the locale actually changed what snprintf() would
		 * produce -- otherwise this "test" would pass vacuously. */
		char probe[32];
		snprintf(probe, sizeof(probe), "%.1f", 1.5);
		assert(strchr(probe, ',') != NULL && "fr_FR locale did not "
		    "change the decimal point -- environment/libc issue, not "
		    "an nvstore bug; investigate before trusting this test");

		unlink(path);
		nvs = nvs_open(path);
		nvs_change_path(nvs, "f");
		nvs_write_float(nvs, "v", val); /* written while LC_NUMERIC=fr_FR */
		nvs_change_path(nvs, "..");
		nvs_close(nvs);

		setlocale(LC_NUMERIC, "C"); /* read back under a DIFFERENT locale */
		nvs = nvs_open(path);
		nvs_change_path(nvs, "f");
		back = nvs_read_float(nvs, "v");
		nvs_close(nvs);
		unlink(path);

		assert(fabs(back - val) < 1e-9);

		/* And the reverse direction: write under C, read under fr_FR. */
		setlocale(LC_NUMERIC, "C");
		unlink(path);
		nvs = nvs_open(path);
		nvs_change_path(nvs, "f");
		nvs_write_float(nvs, "v", val);
		nvs_change_path(nvs, "..");
		nvs_close(nvs);

		setlocale(LC_NUMERIC, "fr_FR.UTF-8");
		nvs = nvs_open(path);
		nvs_change_path(nvs, "f");
		back = nvs_read_float(nvs, "v");
		nvs_close(nvs);
		unlink(path);

		assert(fabs(back - val) < 1e-9);

		setlocale(LC_NUMERIC, "C");
	}
}

int
main(void)
{
	test_fixture_roundtrip();
	test_missing_file_defaults();
	test_escaping();
	test_atomic_save_leaves_no_tmp_file();
	test_corrupt_first_line_indented();
	test_corrupt_depth_jump();
	test_corrupt_stale_slot_reuse();
	test_vector_must_be_closed_before_sibling_scalar();
	test_unrecognized_token_falls_back();
	test_float_locale_roundtrip();
	fprintf(stderr, "test_nvstore: all OK\n");
	return 0;
}
