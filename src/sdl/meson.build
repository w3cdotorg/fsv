# SPDX-License-Identifier: MIT
#
# SDL3 + Metal (SDL_GPU) frontend skeleton (Task 2.2): window, GPU
# device, main loop, ImGui (SDL3 + SDLGPU3 backends), and the core
# animation tick. No scene rendering yet — that's Milestone 3
# (src/sdl/gpu.h/gpu.cpp, Task 3.2).
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

fsv_sdl = executable('fsv', ['main.cpp', headless_stubs_src],
  link_with: libfsvcore,
  dependencies: [sdl3dep, imgui_dep, glibdep, cglm_dep, libm, libmisc_dep, libdebug_dep],
  include_directories: incdir,
  override_options: ['cpp_std=c++20'],
  install: true)
