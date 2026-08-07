# Porting fsv to macOS / Metal

This branch (`metal-port`) replaces the GTK3 + OpenGL frontend with
SDL3 + SDL_GPU (Metal on macOS) + Dear ImGui.

- Plan: [docs/superpowers/plans/2026-08-06-macos-metal-port.md](superpowers/plans/2026-08-06-macos-metal-port.md)
- Upstream: https://github.com/jabl/fsv (tracked on `master`)
- Status: **M0 done — M1 (headless core) in progress (Tasks 1.1, 1.2 done)**

## Task 1.1 verification (platform hooks header)

`src/fsv-platform.h` was added and `src/animation.c`'s animation loop
(`redraw()` / `fsv_animation_tick()`) now goes through `fsv_platform`
hooks instead of calling `g_idle_add_full`/`ogl_draw()` directly. The
GTK frontend installs its hook implementations in `window.c`'s
`window_init()`.

- **macOS (this machine):** installed `meson`, `ninja`, `gtk+3`,
  `libepoxy`, `cglm` via Homebrew and ran
  `meson setup builddir && ninja -C builddir`. `src/animation.c` and
  `src/window.c` (the two files touched by this task) compile cleanly
  with no warnings. The overall build still fails at `src/ogl.c`
  (`fatal error: 'GL/glu.h' file not found`) — confirmed via
  `git stash` that this failure **pre-dates this task's changes**
  (macOS has no `GL/glu.h`; unrelated to platform-hooks work, in scope
  for a later OpenGL-removal task).
  `fsv-platform.h` was also syntax-checked standalone in both C and
  C++ mode (`cc -fsyntax-only -x c` / `c++ -fsyntax-only -x c++`) with
  zero GTK/GL/GLib includes required, confirming the header compiles
  independently with its `extern "C"` guards.
- **Linux (Debian bookworm container via OrbStack/Docker):** installed
  `meson ninja-build pkg-config gcc libgtk-3-dev libepoxy-dev
  libcglm-dev gettext file libglu1-mesa-dev` and ran
  `meson setup builddir && ninja -C builddir` end to end — **full
  build succeeds and links `src/fsv`**, confirming the GTK frontend
  keeps working identically through the new hooks.

## Task 1.2 verification (camera.c off GtkAdjustment)

`src/camera.c` no longer includes `<gtk/gtk.h>` or touches
`GtkAdjustment` at all. It tracks per-axis scroll state (lower, upper,
page, value) in a small static `ScrollState[2]` and pushes/reads it
through `fsv_platform.set_scroll( )`/`get_scroll( )`. The old
`camera_pass_scrollbar_widgets( )` is gone from `camera.h`/`camera.c`;
`camera.h` now declares `camera_scrollbar_moved( int axis )` instead.

The one remaining GTK↔camera wire (user drags a scrollbar → camera
target moves) now lives entirely in `window.c`: a `"value_changed"`
signal handler (`on_scrollbar_value_changed`) connected to each
scrollbar's `GtkAdjustment` in `window_init( )` calls
`camera_scrollbar_moved( axis )`, which reads the new position back via
`fsv_platform.get_scroll( )`. `gtk_set_scroll( )` (the `set_scroll`
hook impl) blocks that same signal handler around its own
`gtk_adjustment_set_*` calls, so the camera's own programmatic scrollbar
pushes (e.g. during a pan) don't loop back into
`camera_scrollbar_moved( )`. This is the single surviving GTK↔camera
path — the old parallel path (camera.c's own `x_scrollbar_adj`/
`y_scrollbar_adj` statics + `camera_pass_scrollbar_widgets( )`) is
deleted. `gui_adjustment_widget_busy( )` (gui.c/gui.h), which had no
other caller after this change, was removed as dead code.

Dropped as part of the cleanup: the widget "busy" throttle that used to
gate how often `camera_update_scrollbars( )` pushed a `"changed"`
signal during a pan (a heuristic explicitly marked "HACK ALERT" in the
original code). `fsv_platform.set_scroll( )` is now called
unconditionally every time; `hard_update` is kept as a parameter (for
API stability — several call sites pass `TRUE`/`FALSE`) but is
otherwise unused. This is a minor behavior change (scrollbar widgets
now get updated on every pan step instead of at most ~18/sec) with no
effect on camera math; traced by reading, not observable without a
GUI.

- **Linux (Debian bookworm container via OrbStack/Docker):** same
  package set as Task 1.1 (`meson ninja-build pkg-config gcc
  libgtk-3-dev libepoxy-dev libcglm-dev gettext file
  libglu1-mesa-dev`). `meson setup builddir && ninja -C builddir`
  succeeds end to end with zero warnings and links `src/fsv`.
- **macOS (this machine):** `export PATH="/opt/homebrew/bin:$PATH"`,
  `meson setup builddir && ninja -C builddir`. `src/camera.c`,
  `src/camera.h`, `src/window.c` and `src/gui.c` (the files touched by
  this task) compile with zero warnings. Build still fails at
  `src/ogl.c` (`GL/glu.h` not found) — the same pre-existing,
  out-of-scope failure noted under Task 1.1.
- `grep -n "gtk\|Gtk\|GTK" src/camera.c` returns no matches.
- Manual GUI testing isn't possible in the container; scrollbar
  behavior was verified by tracing the signal wiring described above
  instead (block/unblock around every programmatic `set_scroll`, one
  `value_changed` handler per axis calling the same camera entry point
  the old `camera_scrollbar_move_cb( )` used).

## Why this architecture

The core of fsv is already cleanly separated: `scanfs.c`, `geometry.c`
(scene building), `camera.c` (math), `colexp.c`, `color.c`, `common.c`
and `animation.c` contain zero GTK widget code. All OpenGL calls live
in just four files (`ogl.c`, `geometry.c`, `tmaptext.c`, `about.c`).
GTK will never grow a
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
