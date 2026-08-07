# SPDX-License-Identifier: MIT
#
# SDL3 + Metal (SDL_GPU) frontend: window, main loop, ImGui (SDL3 +
# SDLGPU3 backends) and the core animation tick (main.cpp, Task 2.2),
# plus the renderer core — device, pipelines, mesh buffers, camera
# matrices (gpu.cpp, Task 3.2). Scene geometry lands in Task 3.3.
#
# The SDL frontend implements only part of the frontend surface the fsv
# core notifies (see the header of src/sdl/stubs.c): the ImGui panels,
# menus and the texture-mapped text engine land in later tasks, so their
# entry points are no-ops for now. geometry.c, by contrast, is compiled
# for real here -- which is why this target uses its own stub file rather
# than tools/fsv-headless-stubs.c, whose geometry_* no-ops would collide.
#
# geometry.c is listed as a source rather than folded into libfsvcore: it
# now calls gpu.h, which each frontend implements, and libfsvcore's other
# consumers (fsv-scan, test_scanfs) have no renderer to link against.
core_gl_src = files('stubs.c', '../geometry.c')

# The compiled shader artifacts (Task 3.1) are baked into the binary as
# byte arrays rather than loaded from disk — see the rationale at the top
# of tools/embed-shaders.py. Only the scene shaders are needed here; the
# text ones join the list in Task 3.4.
python3 = find_program('python3')
embed_shaders_py = files('../../tools/embed-shaders.py')
shaders_embedded_h = custom_target('shaders_embedded.h',
  input: files(
    '../../shaders/compiled/scene.vert.msl',
    '../../shaders/compiled/scene.vert.spv',
    '../../shaders/compiled/scene.frag.msl',
    '../../shaders/compiled/scene.frag.spv'),
  output: 'shaders_embedded.h',
  command: [python3, embed_shaders_py, '@OUTPUT@', '@INPUT@'])

fsv_sdl = executable('fsv',
  ['main.cpp', 'gpu.cpp', shaders_embedded_h, core_gl_src],
  link_with: libfsvcore,
  dependencies: [sdl3dep, imgui_dep, glibdep, cglm_dep, libm, libmisc_dep, libdebug_dep],
  # include_directories('.') resolves to *both* src/sdl and its build
  # directory, which is where shaders_embedded.h is generated.
  include_directories: [incdir, include_directories('.')],
  override_options: ['cpp_std=c++20'],
  install: true)
