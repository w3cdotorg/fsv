# SPDX-License-Identifier: MIT
#
# SDL3 + Metal (SDL_GPU) frontend: window, main loop, ImGui (SDL3 +
# SDLGPU3 backends) and the core animation tick (main.cpp, Task 2.2),
# plus the renderer core — device, pipelines, mesh buffers, camera
# matrices (gpu.cpp, Task 3.2). Scene geometry lands in Task 3.3.
#
# libfsvcore still calls into a handful of frontend-notification
# symbols (gui_update, dirtree_*, filelist_*, geometry_*, window_*,
# viewport_pass_node_table) that belong to the not-yet-ported GTK/GL
# frontend files: fsv_animation_tick() touches the shared `globals`
# struct (common.c), and common.o itself calls gui_update() — so those
# symbols must resolve even though this task never calls into
# scanfs/geometry. Real implementations land task by task (dirtree/
# filelist panels in M5, geometry/viewport in M3); until then, reuse
# the same headless no-op stubs tools/fsv-headless-stubs.c already
# provides for fsv-scan and test_scanfs, as a temporary measure. This
# is the smallest correct choice — they already link cleanly and exist
# for exactly this purpose.
headless_stubs_src = files('../../tools/fsv-headless-stubs.c')

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
  ['main.cpp', 'gpu.cpp', shaders_embedded_h, headless_stubs_src],
  link_with: libfsvcore,
  dependencies: [sdl3dep, imgui_dep, glibdep, cglm_dep, libm, libmisc_dep, libdebug_dep],
  # include_directories('.') resolves to *both* src/sdl and its build
  # directory, which is where shaders_embedded.h is generated.
  include_directories: [incdir, include_directories('.')],
  override_options: ['cpp_std=c++20'],
  install: true)
