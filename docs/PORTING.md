# Porting fsv to macOS / Metal

This branch (`metal-port`) replaces the GTK3 + OpenGL frontend with
SDL3 + SDL_GPU (Metal on macOS) + Dear ImGui.

- Plan: [docs/superpowers/plans/2026-08-06-macos-metal-port.md](superpowers/plans/2026-08-06-macos-metal-port.md)
- Upstream: https://github.com/jabl/fsv (tracked on `master`)
- Status: **M1 (headless core) done — M2 (dependencies) in progress (Task 2.1 done)**

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

## Task 1.3 verification (libfsvcore + fsv-scan + fixture test)

A `frontend` meson option (`gtk`/`sdl`, default `gtk`) was added, but
the three GTK dependency lookups (`gtk+-3.0`, `gdk-pixbuf-2.0`,
`epoxy`) are `required: false` unconditionally — not tied to
`frontend`'s value at all. (An earlier version of this made
`required:` track `frontend == 'gtk'`, which still hard-fails
`meson setup` on a genuinely GTK-less host whenever `frontend` is left
at its `gtk` default — i.e. the common case. Fixed per code review.)
The `fsv` executable target itself only builds when
`frontend == 'gtk'` **and** `gtkdep_found` (all three deps actually
present) **and** `host_machine.system() != 'darwin'` — the last gate
because `ogl.c`'s `GL/glu.h` dependency doesn't exist on macOS
(pre-existing, out of scope here; see Task 1.1). This means
`meson setup` never hard-fails on GTK's absence, on any host, with any
`frontend` value; the headless core (`libfsvcore`, `fsv-scan`,
`test_scanfs`) always configures and builds, and `src/fsv` is silently
skipped whenever GTK isn't there to build it (verified below by
simulating a GTK-less `PKG_CONFIG_PATH`).

`libfsvcore` (`src/meson.build`) is a static library built from
`scanfs.c colexp.c color.c common.c animation.c camera.c` against only
`glib-2.0`, `cglm`, `libm` — plus `libmisc_dep` (nvstore, used by
`color.c`'s wildcard-pattern persistence) and `libdebug_dep` (the
default meson `buildtype` is `debug`, which defines `-DDEBUG`, which
makes `common.h` route `xmalloc`/`xfree`/etc. through `debug.c`'s
allocator-tracking wrappers — those two are pulled in for exactly the
same reason the existing `fsv` executable target already links them).
Neither pulls in GTK: `libdebug`'s own `meson.build` lists `gtkdep` as
a *compile-only* dependency of `debug.c` (which doesn't call any GTK
function — likely dead cruft from an earlier version, left alone as
out of scope for this task), and `declare_dependency()` doesn't
propagate it to consumers.

**Breaking the transitive-GTK trap in `window.h`:** `camera.c`/
`color.c`/`scanfs.c` include `window.h` for GTK-free frontend
notifications (`window_set_access`, `window_birdseye_view_off`,
`window_set_color_mode`, `window_statusbar`), but `window.h`
unconditionally did `#include <gtk/gtk.h>` and declared
`window_init(GtkApplication *, gpointer)` unconditionally too. Fixed
by following the exact convention `gui.h` already uses elsewhere in
this codebase: drop the header's own `<gtk/gtk.h>` include, and gate
`window_init()`'s declaration behind `#ifdef __GTK_H__` (true once a
caller has included `<gtk/gtk.h>` itself first). `window.c` — the only
place besides `fsv.c` that needs `window_init()` visible — had its
`#include "window.h"` reordered to after `#include <gtk/gtk.h>`.
Also removed a dead, unused `#include <gtk/gtk.h>` directly in
`scanfs.c` (scanfs.c calls no GTK symbol itself; it was only there to
paper over `window.h`'s forced include).

**`fsv-scan` / `test_scanfs` link against libfsvcore's objects, not
the library as a whole**, exactly as the task brief anticipated —
but the missing piece the brief didn't call out is that `scanfs.c`,
`colexp.c` and `camera.c` also call into `dirtree.c`, `filelist.c`,
`geometry.c`, `gui.c`, `viewport.c` and `window.c` (all real GTK/GL
frontend files, deliberately *not* part of libfsvcore). Those calls
are all one-way "notify the frontend" hooks (redraw a widget, update a
status bar, recompute cached 3D geometry) with no return value the
core logic depends on. `tools/fsv-headless-stubs.c` provides headless
no-op implementations of exactly those 30 symbols
(`gui_update`, `dirtree_*`, `filelist_*`, `geometry_*`,
`viewport_pass_node_table`, `window_*`), shared by both `fsv-scan` and
`test_scanfs` via a `headless_stubs_src` variable set in
`tools/meson.build`. This file is *not* part of `libfsvcore` — real
frontends keep providing their own real implementations.

`tests/fixture/` is a tiny committed tree (`file1.txt`, `dir-a/`,
`dir-a/file2.bin`, `dir-a/dir-b/`, `dir-a/dir-b/file3`) and
`tests/test_scanfs.c` asserts `scanfs(FIXTURE_DIR)` produces a
non-null `globals.fstree` with `g_node_n_nodes(..., G_TRAVERSE_ALL) >=
6`. `FIXTURE_DIR` is injected via `c_args`.

TDD evidence:
- **RED:** before `tools/meson.build`/`tests/meson.build` existed,
  `ninja -C builddir test_scanfs fsv-scan` → `ninja: error: unknown
  target 'test_scanfs'`. After wiring the meson targets but before
  fixing `window.h`, the build failed to *compile*
  (`fatal error: 'gtk/gtk.h' file not found` in `color.c`/`camera.c`/
  `scanfs.c`). After fixing `window.h`/`scanfs.c`, it failed to
  *link* (`ld: symbol(s) not found` for the 30 frontend-notification
  symbols listed above).
- **GREEN:** after adding `tools/fsv-headless-stubs.c` and linking it
  into both targets, `ninja -C builddir` builds cleanly and
  `meson test -C builddir scanfs` → `1/1 fsv:scanfs OK`.

- **macOS (this machine, default `meson setup builddir`, buildtype
  `debug`):** `ninja -C builddir` builds all 8 targets (`libfsvcore`,
  `fsv-scan`, `test_scanfs`, plus `libmisc`/`libdebug`/po/gresource
  helpers) with zero touch of GTK — no `src/fsv` target is even
  generated. `meson test -C builddir scanfs` → `1/1 fsv:scanfs OK`.
  `./builddir/tools/fsv-scan tests/fixture` → `nodes=7`, exit 0. Note:
  this meson version (1.11.2) doesn't create bare `ninja` aliases
  `test_scanfs`/`fsv-scan` as the brief's exact command assumed — use
  `ninja -C builddir` (build everything) or the full target paths
  `tools/fsv-scan` / `tests/test_scanfs`; `meson test` resolves the
  test target internally regardless.
- **Linux (Debian bookworm container via OrbStack/Docker, same package
  set as Tasks 1.1/1.2):** `meson setup builddir && ninja -C builddir`
  builds all 11 targets end to end, **including `src/fsv`** (the GTK
  frontend keeps building identically — `frontend` defaults to `gtk`
  and the host isn't macOS). `meson test -C builddir scanfs` → `1/1
  scanfs OK`. `./builddir/tools/fsv-scan tests/fixture` → `nodes=7`.

## Task 2.1 verification (SDL3 + vendored Dear ImGui subproject)

**SDL3:** already present via Homebrew on this machine —
`pkg-config --modversion sdl3` → `3.4.14` (≥ 3.2 required for the
SDLGPU3 backend). `dependency('sdl3')` resolves via pkg-config with
plain `/opt/homebrew/bin` on `PATH`; no extra `PKG_CONFIG_PATH` needed
on this host (Homebrew's `pkg-config` already indexes
`/opt/homebrew/lib/pkgconfig`). If a future host's `pkg-config` can't
see it, point `PKG_CONFIG_PATH` at `$(brew --prefix sdl3)/lib/pkgconfig`.

**Dear ImGui:** vendored (copied, not a submodule) into
`subprojects/imgui/` from the **docking** branch at tag
`v1.92.9b-docking` (commit `b48d1afbe8ee8b238e2961dc363a949dd7304e23`
— the latest docking tag at the time of this task; well above the
1.91.6 floor where the SDLGPU3 backend first shipped). Only the files
needed to build with the SDL3 + SDLGPU3 backends were copied: the 5
compiled `.cpp` core files (`imgui.cpp`, `imgui_draw.cpp`,
`imgui_tables.cpp`, `imgui_widgets.cpp`, `imgui_demo.cpp` — the demo
window is kept in because Task 2.2 calls `ImGui::ShowDemoWindow()`),
headers (`imgui.h`, `imgui_internal.h`, `imconfig.h`, the three
`imstb_*.h` single-header deps), `LICENSE.txt`, and the
`backends/imgui_impl_sdl3.{h,cpp}` /
`backends/imgui_impl_sdlgpu3.{h,cpp,_shaders.h}` pair. No
`examples/`, `docs/`, `misc/` or `.github/` from upstream. See
`subprojects/imgui/VERSION.txt` for the exact provenance and update
instructions. `examples/example_sdl3_sdlgpu3/main.cpp` was copied
alongside (renamed `example_sdl3_sdlgpu3_main.cpp.txt`, so it's never
picked up by the build) purely as an API reference for Task 2.2.

`subprojects/imgui/meson.build` declares its own `project('imgui',
'cpp', default_options: ['cpp_std=c++20'])` — deliberately isolated
from the root project's C11 toolchain — and builds a `static_library`
+ `declare_dependency()` named `imgui_dep` (also carrying the `sdl3`
pkg-config dependency, so anything depending on `imgui_dep` gets
SDL3's include path for free, since `imgui_impl_sdl3.h` needs
`<SDL3/SDL.h>`).

Root `meson.build` gates the subproject exactly like `gtkdep`/
`gtkdep_found` gates the GTK frontend: `sdl3dep = dependency('sdl3',
required: false)` (never hard-required at configure time — a host
without SDL3 must still configure the headless core), and
`subproject('imgui')` is only executed when `frontend == 'sdl' and
sdl3dep.found()`. This is temporary wiring for this task only (proves
the subproject builds); Task 2.2 is what actually consumes
`imgui_dep`/`sdl3dep` from a real `src/sdl/` target.

- **macOS (this machine):** `meson setup builddir-imgui-check
  -Dfrontend=sdl` configures cleanly — log shows `Executing subproject
  imgui`, `Subprojects: imgui: YES`. `meson compile -C
  builddir-imgui-check` builds all 7 imgui object files
  (`imgui.cpp.o`, `imgui_draw.cpp.o`, `imgui_tables.cpp.o`,
  `imgui_widgets.cpp.o`, `imgui_demo.cpp.o`,
  `backends_imgui_impl_sdl3.cpp.o`, `backends_imgui_impl_sdlgpu3.cpp.o`)
  and links `subprojects/imgui/libimgui.a` with **zero errors** (a few
  upstream `-Wall` warnings from clang are expected and were not
  patched, per the task's own constraint) — with no target anywhere
  depending on `imgui_dep` yet; a bare `subproject('imgui')` call is
  enough because `static_library()`'s targets are `build_by_default`.
  The headless core keeps building/testing in the same tree:
  `meson test -C builddir-imgui-check scanfs` → `1/1 fsv:scanfs OK`.
- **GTK path untouched:** `meson setup builddir-gtkcheck` (default,
  `frontend=gtk`) configures 8 targets with no `Executing subproject`
  line and no `builddir-gtkcheck/subprojects/` directory at all;
  `meson test -C builddir-gtkcheck scanfs` → `1/1 fsv:scanfs OK`.
- Both scratch build directories were deleted after verification (not
  committed) — same convention as the `builddir` used in earlier
  tasks.

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
| 2026-08-07 | Vendor ImGui `v1.92.9b-docking` by file-copy, not git submodule | latest docking tag at task time; a copy keeps `subprojects/imgui/` a normal, reviewable part of the tree with no upstream history/examples/docs bloat |
