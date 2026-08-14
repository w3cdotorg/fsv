# SPDX-License-Identifier: MIT

# Shared bits factored out for every test that exercises libfsvcore
# headlessly (all of them except nvstore's stub-free unit test):
# core_test_deps is the common dependency list, fixture_c_args the
# fixture path -Ddefine. objects: (extract_all_objects) and
# include_directories: are still repeated per executable() below.
core_test_deps = [glibdep, cglm_dep, libm, libmisc_dep, libdebug_dep]
fixture_c_args = ['-DFIXTURE_DIR="@0@"'.format(meson.current_source_dir() / 'fixture')]

test_scanfs = executable('test_scanfs', ['test_scanfs.c', headless_stubs_src],
  objects: libfsvcore.extract_all_objects(recursive: false),
  dependencies: core_test_deps,
  include_directories: incdir,
  c_args: fixture_c_args)
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
  dependencies: core_test_deps,
  include_directories: incdir)
test('color_persistence', test_color_persistence)

# fsn-mode Task B1: FSV_FSN's layout pass is deliberately gpu-free, which
# is what puts src/geometry-fsn.c in libfsvcore -- so this links exactly
# like test_scanfs above, with no renderer stubs of any kind.
test_fsn_layout = executable('test_fsn_layout',
  ['test_fsn_layout.c', headless_stubs_src],
  objects: libfsvcore.extract_all_objects(recursive: false),
  dependencies: core_test_deps,
  include_directories: incdir,
  c_args: fixture_c_args)
test('fsn_layout', test_fsn_layout)

# Regression lock for the FSN collapsed-parent camera fix (fa9383c,
# fsn_ensure_parent_expanded() -- TODO.md "Known bugs" item 1):
# camera_look_at()/camera_warp_to() on a node whose parent directory is
# flag-collapsed must pre-expand it instead of tripping their DEBUG
# assertion. Depends on the headless dirtree stubs being stateful.
test_fsn_camera = executable('test_fsn_camera',
  ['test_fsn_camera.c', headless_stubs_src],
  objects: libfsvcore.extract_all_objects(recursive: false),
  dependencies: core_test_deps,
  include_directories: incdir,
  c_args: fixture_c_args)
test('fsn_camera', test_fsn_camera)

# Regression test for TODO.md bug #2 (fsn_look_at()'s nose-first
# file-zoom framing): a file must be framed at its parent directory's
# establishing distance -- target on the file, distance from the parent
# pedestal's footprint -- not at its own box-scale footprint.
test_fsn_framing = executable('test_fsn_framing',
  ['test_fsn_framing.c', headless_stubs_src],
  objects: libfsvcore.extract_all_objects(recursive: false),
  dependencies: core_test_deps,
  include_directories: incdir,
  c_args: fixture_c_args)
test('fsn_framing', test_fsn_framing)

# Pure squarified-treemap module (src/squarify.c) -- MapV's layout
# algorithm, extracted precisely so it can be tested here without the
# GL-bound geometry.c around it.
test_squarify = executable('test_squarify',
  ['test_squarify.c', headless_stubs_src],
  objects: libfsvcore.extract_all_objects(recursive: false),
  dependencies: core_test_deps,
  include_directories: incdir)
test('squarify', test_squarify)

# Built-in scan exclusion (scanfs.c): excluded dir names never enter
# the tree; files with those names always do; the toggle restores full
# scanning. Builds its own mkdtemp tree (a committed .git fixture dir
# would fight git itself).
test_scanfs_exclude = executable('test_scanfs_exclude',
  ['test_scanfs_exclude.c', headless_stubs_src],
  objects: libfsvcore.extract_all_objects(recursive: false),
  dependencies: core_test_deps,
  include_directories: incdir)
test('scanfs_exclude', test_scanfs_exclude)

# Short-arc theta on fixed-heading re-poses after a manual revolve
# (TODO.md's last Known bug): DiscV/MapV/TreeV get the same
# unwrap_theta_toward() treatment FSN's Task B2 fix round introduced.
test_camera_theta = executable('test_camera_theta',
  ['test_camera_theta.c', headless_stubs_src],
  objects: libfsvcore.extract_all_objects(recursive: false),
  dependencies: core_test_deps,
  include_directories: incdir,
  c_args: fixture_c_args)
test('camera_theta', test_camera_theta)
