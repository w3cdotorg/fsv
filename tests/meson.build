# SPDX-License-Identifier: MIT

test_scanfs = executable('test_scanfs', ['test_scanfs.c', headless_stubs_src],
  objects: libfsvcore.extract_all_objects(recursive: false),
  dependencies: [glibdep, cglm_dep, libm, libmisc_dep, libdebug_dep],
  include_directories: incdir,
  c_args: ['-DFIXTURE_DIR="@0@"'.format(meson.current_source_dir() / 'fixture')])
test('scanfs', test_scanfs)

# lib/nvstore.c gained real logic in Task 5.3 of the SDL/Metal port
# (was a complete stub before); worth its own test independent of
# color.c/libfsvcore -- only needs libmisc_dep (which already declares
# lib/'s own include_directories) and libm (fabs(), for the float
# round-trip check).
test_nvstore = executable('test_nvstore', ['test_nvstore.c'],
  dependencies: [libm, libmisc_dep])
test('nvstore', test_nvstore)

# Regression test for the Critical-2 fix-round bug (a missing
# nvs_vector_end() in color_read_config() before its defaultcolor
# read) -- exercises the real src/color.c entry points end to end,
# same headless-linking approach as test_scanfs above.
test_color_persistence = executable('test_color_persistence',
  ['test_color_persistence.c', headless_stubs_src],
  objects: libfsvcore.extract_all_objects(recursive: false),
  dependencies: [glibdep, cglm_dep, libm, libmisc_dep, libdebug_dep],
  include_directories: incdir)
test('color_persistence', test_color_persistence)

# fsn-mode Task B1: FSV_FSN's layout pass is deliberately gpu-free so it
# can be checked without a renderer. src/geometry-fsn.c is compiled in
# directly (it is frontend-side, like geometry.c, and so is absent from
# libfsvcore); the test file itself supplies the handful of gpu.h/
# tmaptext.h no-ops its *draw* pass needs to link -- see its header.
test_fsn_layout = executable('test_fsn_layout',
  ['test_fsn_layout.c', '../src/geometry-fsn.c', headless_stubs_src],
  objects: libfsvcore.extract_all_objects(recursive: false),
  dependencies: [glibdep, cglm_dep, libm, libmisc_dep, libdebug_dep],
  include_directories: incdir,
  c_args: ['-DFIXTURE_DIR="@0@"'.format(meson.current_source_dir() / 'fixture')])
test('fsn_layout', test_fsn_layout)
