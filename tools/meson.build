# SPDX-License-Identifier: MIT

# Headless no-op implementations of the frontend-notification functions
# libfsvcore calls into (dirtree_*, filelist_*, geometry_*, gui_update,
# viewport_pass_node_table, window_*). Shared with tests/test_scanfs so
# both can link libfsvcore standalone, without any GTK/GL frontend code.
headless_stubs_src = files('fsv-headless-stubs.c')

# fsv-scan links directly against the libfsvcore objects (not the frontend
# executable's object list) so it stays GTK/GL-free.
fsv_scan = executable('fsv-scan', ['fsv-scan.c', headless_stubs_src],
  objects: libfsvcore.extract_all_objects(recursive: false),
  dependencies: [glibdep, cglm_dep, libm, libmisc_dep, libdebug_dep],
  include_directories: incdir)
