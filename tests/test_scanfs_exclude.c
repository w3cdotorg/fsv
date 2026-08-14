/* tests/test_scanfs_exclude.c
 *
 * SPDX-License-Identifier: MIT
 *
 * The built-in scan exclusion (upstream-1999 TODO "a way to exclude
 * directories from the scan"; today's everyday case is .git/build
 * dirs dominating MapV's byte-proportional layout). Directories whose
 * basename is on scanfs.c's built-in list are never traversed and
 * never enter the tree; FILES with those names are never excluded;
 * the toggle restores full scanning. The tree is built here at
 * runtime (mkdtemp) rather than in tests/fixture -- a committed
 * directory literally named .git inside the fixture would fight git
 * itself.
 *
 * LINKING: same recipe as test_scanfs (scanfs.c is in libfsvcore). */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h> /* mkdtemp() -- declared here, not stdlib.h, on macOS */

#include "common.h" /* pulls in glib.h, and must precede it: G_LOG_DOMAIN */
#include "fsv.h"
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

int
main(void)
{
	char tmpl[] = "/tmp/fsv-exclude-XXXXXX";
	char sub[1024];
	char *root;
	GNode *dnode;

	root = mkdtemp(tmpl);
	assert(root != NULL);

	/* .git/ with content (the thing to skip) */
	snprintf(sub, sizeof(sub), "%s/.git", root);
	assert(mkdir(sub, 0755) == 0);
	make_file(sub, "pack.bin", 4096);
	/* node_modules/ with content */
	snprintf(sub, sizeof(sub), "%s/node_modules", root);
	assert(mkdir(sub, 0755) == 0);
	make_file(sub, "dep.js", 512);
	/* normal content: a dir and files */
	snprintf(sub, sizeof(sub), "%s/src", root);
	assert(mkdir(sub, 0755) == 0);
	make_file(sub, "main.c", 100);
	make_file(root, "README", 50);
	/* a FILE named .git -- must never be excluded (dirs only) */
	snprintf(sub, sizeof(sub), "%s/src", root);
	make_file(sub, ".git", 10);

	/* Exclusion ON (the default) */
	assert(scanfs_get_exclusion());
	scanfs(root);
	assert(globals.fstree != NULL);
	dnode = root_dnode;
	assert(dnode != NULL);
	assert(child_named(dnode, ".git") == NULL);
	assert(child_named(dnode, "node_modules") == NULL);
	assert(child_named(dnode, "README") != NULL);
	assert(child_named(dnode, "src") != NULL);
	/* the FILE named .git inside src/ survives */
	assert(child_named(child_named(dnode, "src"), ".git") != NULL);
	assert(!NODE_IS_DIR(child_named(child_named(dnode, "src"), ".git")));

	/* Exclusion OFF: everything scans */
	scanfs_set_exclusion(FALSE);
	scanfs(root);
	dnode = root_dnode;
	assert(child_named(dnode, ".git") != NULL);
	assert(NODE_IS_DIR(child_named(dnode, ".git")));
	assert(child_named(child_named(dnode, ".git"), "pack.bin") != NULL);
	assert(child_named(dnode, "node_modules") != NULL);

	return 0;
}
