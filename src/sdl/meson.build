# SPDX-License-Identifier: MIT
#
# SDL3 + Metal (SDL_GPU) frontend: window, main loop, ImGui (SDL3 +
# SDLGPU3 backends) and the core animation tick (main.cpp, Task 2.2),
# plus the renderer core — device, pipelines, mesh buffers, camera
# matrices (gpu.cpp, Task 3.2). Scene geometry lands in Task 3.3; the
# texture-mapped text engine (tmaptext.c) in Task 3.4.
#
# The SDL frontend implements only part of the frontend surface the fsv
# core notifies (see the header of src/sdl/stubs.c): the menu bar
# (ui_main.cpp, Task 5.1) and the dirtree/filelist panels (ui_panels.cpp,
# Task 5.2) are both real now. geometry.c and tmaptext.c, by contrast,
# are compiled for real here -- which is why this target uses its own
# stub file rather than tools/fsv-headless-stubs.c, whose geometry_*/
# text_* no-ops would collide.
#
# Neither file is folded into libfsvcore: both now call gpu.h, which each
# frontend implements, and libfsvcore's other consumers (fsv-scan,
# test_scanfs) have no renderer to link against.
# geometry-fsn-draw.c (fsn-mode Task B1) rides along with geometry.c for
# exactly the same reason: it is FSV_FSN's share of the same per-mode
# geometry surface, split into its own file only because geometry.c is
# already ~2.9k lines. Its *layout* counterpart, geometry-fsn.c, is
# gpu-free and lives in libfsvcore instead (see src/meson.build).
core_gl_src = files('stubs.c', '../geometry.c', '../geometry-fsn-draw.c',
                    '../tmaptext.c', '../fontatlas.c')

# The compiled shader artifacts (Task 3.1) are baked into the binary as
# byte arrays rather than loaded from disk — see the rationale at the top
# of tools/embed-shaders.py.
python3 = find_program('python3')
embed_shaders_py = files('../../tools/embed-shaders.py')
shaders_embedded_h = custom_target('shaders_embedded.h',
  input: files(
    '../../shaders/compiled/scene.vert.msl',
    '../../shaders/compiled/scene.vert.spv',
    '../../shaders/compiled/scene.frag.msl',
    '../../shaders/compiled/scene.frag.spv',
    '../../shaders/compiled/text.vert.msl',
    '../../shaders/compiled/text.vert.spv',
    '../../shaders/compiled/text.frag.msl',
    '../../shaders/compiled/text.frag.spv'),
  output: 'shaders_embedded.h',
  command: [python3, embed_shaders_py, '@OUTPUT@', '@INPUT@'])

fsv_sdl = executable('fsv',
  ['main.cpp', 'gpu.cpp', 'input.cpp', 'ui_main.cpp', 'ui_panels.cpp',
   'ui_dialogs.cpp', 'ui_rail.cpp', shaders_embedded_h, core_gl_src],
  link_with: libfsvcore,
  dependencies: [sdl3dep, imgui_dep, glibdep, cglm_dep, libm, libmisc_dep, libdebug_dep],
  # include_directories('.') resolves to *both* src/sdl and its build
  # directory, which is where shaders_embedded.h is generated.
  include_directories: [incdir, include_directories('.')],
  override_options: ['cpp_std=c++20'],
  install: true)
