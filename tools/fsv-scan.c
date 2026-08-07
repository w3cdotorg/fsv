/* tools/fsv-scan.c
 *
 * SPDX-License-Identifier: MIT
 *
 * fsv-scan: headless smoke test for the fsv core scanner.
 * Usage: fsv-scan <directory>
 */

#include <stdio.h>
#include <glib.h>
#include "common.h"
#include "fsv.h"
#include "scanfs.h"

int
main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <directory>\n", argv[0]);
		return 2;
	}

	scanfs(argv[1]);
	GNode *root = globals.fstree;
	if (root == NULL) {
		fprintf(stderr, "scan produced no tree\n");
		return 1;
	}
	printf("nodes=%u\n", g_node_n_nodes(root, G_TRAVERSE_ALL));
	return 0;
}
