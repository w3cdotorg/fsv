/* tests/test_fscache.c
 *
 * SPDX-License-Identifier: MIT
 *
 * Scan cache (docs/superpowers/specs/2026-08-21-scan-cache-design.md;
 * upstream-1999 TODO "scan caching"). Phase A exercises the fscache
 * module surface directly: save a scanned tree, load it back, walk the
 * index against the live GNode tree, and prove every rejection path
 * (disabled, corrupt file, wrong root, wrong exclusion fingerprint,
 * one-shot skip) falls back to "no cache" instead of failing the scan.
 * Phase B (same file, below) exercises scanfs( )'s replay integration:
 * unchanged directories replay from cache without scandir/lstat, changed
 * directories re-scan, and the in-place-edit staleness contract holds.
 *
 * The tree is built at runtime (mkdtemp), with every directory's mtime
 * backdated after setup: mtimes have second granularity, and the loader's
 * same-second race guard deliberately distrusts directories modified
 * within a second of the cache's own timestamp -- backdating keeps these
 * tests deterministic instead of sleep()-flaky.
 *
 * LINKING: same recipe as test_scanfs (scanfs.c/fscache.c are in
 * libfsvcore). FSV_CACHE_DIR points the cache into the tmp tree.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h> /* mkdtemp() -- declared here, not stdlib.h, on macOS */

#include "common.h" /* pulls in glib.h, and must precede it: G_LOG_DOMAIN */
#include "fsv.h"
#include "fscache.h"
#include "scanfs.h"

static GNode *
child_named(GNode *parent, const char *name)
{
	GNode *node;

	for (node = parent->children; node != NULL; node = node->next)
		if (strcmp(NODE_DESC(node)->name, name) == 0)
			return node;

	return NULL;
}

static void
make_file(const char *dir, const char *name, int bytes)
{
	char path[1024];
	FILE *fp;
	int i;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	fp = fopen(path, "w");
	assert(fp != NULL);
	for (i = 0; i < bytes; i++)
		fputc('x', fp);
	fclose(fp);
}

/* Push a path's mtime well into the past so any later modification
 * observably moves it (second-granularity mtimes + the loader's
 * same-second race guard). */
static void
backdate(const char *path)
{
	struct timeval tv[2];

	gettimeofday(&tv[0], NULL);
	tv[0].tv_sec -= 100;
	tv[0].tv_usec = 0;
	tv[1] = tv[0];
	assert(utimes(path, tv) == 0);
}

static void
backdate_tree(const char *root)
{
	char sub[1024];
	static const char *dirs[] = { "", "/alpha", "/alpha/inner", "/beta" };
	int i;

	for (i = 0; i < (int)G_N_ELEMENTS(dirs); i++) {
		snprintf(sub, sizeof(sub), "%s%s", root, dirs[i]);
		backdate(sub);
	}
}

/* Recursive compare of the cache index against the live scanned tree:
 * same entries, same scalars. */
static void
compare_dir(const FscacheDir *cdir, GNode *dnode)
{
	GNode *node;
	int live_count = 0;

	for (node = dnode->children; node != NULL; node = node->next) {
		const FscacheEntry *ent =
		    fscache_dir_entry_by_name(cdir, NODE_DESC(node)->name);

		++live_count;
		assert(ent != NULL);
		assert(ent->type == NODE_DESC(node)->type);
		assert(ent->size == NODE_DESC(node)->size);
		assert(ent->size_alloc == NODE_DESC(node)->size_alloc);
		assert(ent->mtime == NODE_DESC(node)->mtime);
		assert(ent->ctime == NODE_DESC(node)->ctime);
		if (NODE_IS_DIR(node)) {
			assert(ent->subdir != NULL);
			compare_dir(ent->subdir, node);
		}
		else
			assert(ent->subdir == NULL);
	}
	assert(fscache_dir_entry_count(cdir) == live_count);
}

int
main(void)
{
	char tmpl[] = "/tmp/fsv-fscache-XXXXXX";
	char sub[1024], cachedir[1024], other[1024];
	char *root;
	GNode *dnode;
	time_t t0;

	root = mkdtemp(tmpl);
	assert(root != NULL);

	/* Cache directory, isolated per test run */
	snprintf(cachedir, sizeof(cachedir), "%s/.cachedir", root);
	assert(mkdir(cachedir, 0755) == 0);
	assert(setenv("FSV_CACHE_DIR", cachedir, 1) == 0);

	/* The scanned content:
	 *   alpha/           (a.txt, b.txt, inner/(deep.txt))
	 *   beta/            (c.txt)
	 *   top.txt
	 */
	snprintf(sub, sizeof(sub), "%s/alpha", root);
	assert(mkdir(sub, 0755) == 0);
	make_file(sub, "a.txt", 100);
	make_file(sub, "b.txt", 200);
	snprintf(sub, sizeof(sub), "%s/alpha/inner", root);
	assert(mkdir(sub, 0755) == 0);
	make_file(sub, "deep.txt", 300);
	snprintf(sub, sizeof(sub), "%s/beta", root);
	assert(mkdir(sub, 0755) == 0);
	make_file(sub, "c.txt", 400);
	make_file(root, "top.txt", 50);
	backdate_tree(root);

	/* ---- Phase A: module surface ---------------------------------- */

	/* A1: scan -> save happens inside scanfs() -> file exists; a fresh
	 * load returns a usable index matching the live tree. */
	t0 = time(NULL);
	scanfs(root);
	assert(globals.fstree != NULL);
	dnode = root_dnode;
	assert(dnode != NULL);
	{
		/* scanfs() chdir()'d into root; the cache file is named after
		 * the resolved absolute root. Copied: xgetcwd() hands back its
		 * own static buffer, which every later scanfs() call inside
		 * this test reallocs -- holding the raw pointer here was a
		 * use-after-free that made this test flaky (main.cpp's
		 * xstrdup(xgetcwd()) exists for the same reason). */
		char *resolved = xstrdup(xgetcwd());
		GDir *gd = g_dir_open(cachedir, 0, NULL);
		const char *fname;
		int cache_files = 0;

		assert(gd != NULL);
		while ((fname = g_dir_read_name(gd)) != NULL) {
			assert(fname[0] == '@');
			++cache_files;
		}
		g_dir_close(gd);
		assert(cache_files == 1);

		assert(fscache_load(resolved));
		assert(fscache_prev_scan_time() >= t0);
		assert(fscache_root() != NULL);
		compare_dir(fscache_root(), dnode);

		/* A4: wrong root -> rejected. (Sibling path that exists but
		 * was never scanned.) */
		snprintf(other, sizeof(other), "%s/alpha", resolved);
		fscache_release();
		assert(!fscache_load(other));
		assert(fscache_root() == NULL);
		assert(fscache_prev_scan_time() == 0);

		/* A5: exclusion fingerprint mismatch -> rejected. */
		scanfs_add_exclude_pattern("zzz-not-there-*");
		assert(!fscache_load(resolved));
		/* ...and a re-scan under the new fingerprint re-caches. */
		scanfs(root);
		assert(fscache_load(resolved));
		fscache_release();

		/* A6: one-shot skip (Rescan read-side bypass). */
		fscache_skip_next_load();
		assert(!fscache_load(resolved));
		assert(fscache_load(resolved));
		fscache_release();

		/* A3: corrupt cache -> rejected, not crashed. */
		{
			GDir *gd2 = g_dir_open(cachedir, 0, NULL);
			char cachefile[2048];

			assert(gd2 != NULL);
			fname = g_dir_read_name(gd2);
			assert(fname != NULL);
			snprintf(cachefile, sizeof(cachefile), "%s/%s",
			    cachedir, fname);
			g_dir_close(gd2);
			assert(truncate(cachefile, 40) == 0);
			assert(!fscache_load(resolved));
		}

		/* A2: disabled -> no save, no load. */
		fscache_set_enabled(FALSE);
		scanfs(root); /* would re-save if enabled */
		assert(!fscache_load(resolved));
		fscache_set_enabled(TRUE);
		/* cache file is still the truncated husk: prove a fresh scan
		 * (enabled again) overwrites it into a loadable state. */
		scanfs(root);
		assert(fscache_load(resolved));
		fscache_release();
	}

	printf("test_fscache: phase A OK\n");

	/* ---- Phase B: scanfs() replay integration --------------------- */

	/* B1: nothing changed -> every directory replays; trees identical. */
	backdate_tree(root);
	scanfs(root); /* re-cache with backdated dir mtimes */
	scanfs(root);
	dnode = root_dnode;
	assert(fscache_replayed_dirs() >= 4); /* root, alpha, inner, beta */
	assert(child_named(dnode, "top.txt") != NULL);
	assert(NODE_DESC(child_named(child_named(dnode, "alpha"), "a.txt"))->size == 100);
	assert(NODE_DESC(child_named(child_named(child_named(dnode, "alpha"), "inner"), "deep.txt"))->size == 300);

	/* B2: a new file bumps its directory's mtime -> that directory
	 * re-scans and shows it; siblings still replay. */
	snprintf(sub, sizeof(sub), "%s/beta", root);
	make_file(sub, "d.txt", 40);
	scanfs(root);
	dnode = root_dnode;
	assert(child_named(child_named(dnode, "beta"), "d.txt") != NULL);
	assert(fscache_replayed_dirs() >= 2); /* alpha, inner (at least) */

	/* B3: the staleness contract -- an in-place edit inside an
	 * unchanged directory is invisible until a fresh scan. Rewriting an
	 * existing file moves the FILE's mtime/ctime but neither of its
	 * parent directory's (no directory entry changed), so alpha still
	 * replays from the cache B2's scan just saved. No utimes() here:
	 * utimes would bump alpha's ctime to now and defeat the very match
	 * this case exists to exercise. */
	snprintf(sub, sizeof(sub), "%s/alpha", root);
	make_file(sub, "a.txt", 999);
	scanfs(root);
	dnode = root_dnode;
	assert(NODE_DESC(child_named(child_named(dnode, "alpha"), "a.txt"))->size == 100); /* stale, by design */
	fscache_skip_next_load(); /* Rescan semantics */
	scanfs(root);
	dnode = root_dnode;
	assert(NODE_DESC(child_named(child_named(dnode, "alpha"), "a.txt"))->size == 999);
	assert(fscache_replayed_dirs() == 0);

	printf("test_fscache: phase B OK\n");

	return 0;
}
