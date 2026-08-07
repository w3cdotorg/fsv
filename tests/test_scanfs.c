/* tests/test_scanfs.c
 *
 * SPDX-License-Identifier: MIT
 *
 * Fixture-based unit test for the headless fsv core scanner. Asserts that
 * scanfs() on tests/fixture produces a non-null tree with at least 6 nodes
 * (metanode + fixture root + dir-a + dir-b + file1.txt + file2.bin + file3).
 */

#include <assert.h>
#include <glib.h>
#include "common.h"
#include "fsv.h"
#include "scanfs.h"

int
main(void)
{
	scanfs(FIXTURE_DIR);  /* FIXTURE_DIR injected by meson (-D) */
	assert(globals.fstree != NULL);
	/* 3 files + fixture root + dir-a + dir-b (+ implicit metanode) */
	assert(g_node_n_nodes(globals.fstree, G_TRAVERSE_ALL) >= 6);
	return 0;
}
