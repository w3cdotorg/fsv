# Porting fsv to macOS / Metal

This branch (`metal-port`) replaces the GTK3 + OpenGL frontend with
SDL3 + SDL_GPU (Metal on macOS) + Dear ImGui.

- Plan: [docs/superpowers/plans/2026-08-06-macos-metal-port.md](superpowers/plans/2026-08-06-macos-metal-port.md)
- Upstream: https://github.com/jabl/fsv (tracked on `master`)
- Status: **M0 done — M1 (headless core) next**

## Why this architecture

The core of fsv is already cleanly separated: `scanfs.c`, `geometry.c`
(scene building), `camera.c` (math), `colexp.c`, `color.c`, `common.c`
and `animation.c` contain zero GTK widget code (camera.c only touches
GtkAdjustment for scrollbars). All OpenGL calls live in just four files
(`ogl.c`, `geometry.c`, `tmaptext.c`, `about.c`). GTK will never grow a
Metal backend, so the frontend is replaced wholesale while ~70% of the
code is kept.

## Decision log

| Date | Decision | Why |
|---|---|---|
| 2026-08-06 | SDL3 GPU API over raw Metal | C API grafts naturally onto a C codebase; Metal backend is native on macOS; Vulkan keeps Linux viable |
| 2026-08-06 | Dear ImGui for all 2D UI | replaces GTK menus/panels/dialogs; has official SDL3 + SDLGPU3 backends |
| 2026-08-06 | Check compiled shaders (MSL + SPIR-V) into the repo | avoids SDL_shadercross as a contributor build dependency |
| 2026-08-06 | Keep GLib, drop GTK | core relies on GNode/GList; GLib is headless-safe and available via Homebrew |
| 2026-08-06 | Core stays C11, new frontend files are C++20 | ImGui is C++; core headers get `extern "C"` guards |
| 2026-08-06 | Work on `metal-port`, keep `master` pristine | painless upstream sync with jabl/fsv |
