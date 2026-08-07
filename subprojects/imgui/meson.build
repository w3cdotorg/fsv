# SPDX-License-Identifier: MIT
#
# Vendored Dear ImGui (docking branch, v1.92.9b-docking — see VERSION.txt)
# with the SDL3 + SDLGPU3 backends. Built as a standalone Meson
# subproject so the fsv root build can `subproject('imgui')` /
# `dependency('imgui', fallback: ...)` from the sdl frontend arm without
# imgui's C++20 requirement leaking into the (C11) headless core.
project('imgui', 'cpp', default_options: ['cpp_std=c++20'])

sdl3_dep = dependency('sdl3')

imgui_lib = static_library('imgui',
  [
    'imgui.cpp',
    'imgui_draw.cpp',
    'imgui_tables.cpp',
    'imgui_widgets.cpp',
    'imgui_demo.cpp',
    'backends/imgui_impl_sdl3.cpp',
    'backends/imgui_impl_sdlgpu3.cpp',
  ],
  include_directories: ['.', 'backends'],
  dependencies: sdl3_dep,
)

imgui_dep = declare_dependency(
  link_with: imgui_lib,
  include_directories: ['.', 'backends'],
  dependencies: sdl3_dep,
)
