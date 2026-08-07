# SPDX-License-Identifier: MIT

test_scanfs = executable('test_scanfs', ['test_scanfs.c', headless_stubs_src],
  objects: libfsvcore.extract_all_objects(recursive: false),
  dependencies: [glibdep, cglm_dep, libm, libmisc_dep, libdebug_dep],
  include_directories: incdir,
  c_args: ['-DFIXTURE_DIR="@0@"'.format(meson.current_source_dir() / 'fixture')])
test('scanfs', test_scanfs)
