# Porting fsv to macOS / Metal

This branch (`metal-port`) replaces the GTK3 + OpenGL frontend with
SDL3 + SDL_GPU (Metal on macOS) + Dear ImGui.

## Retrospective (2026-08-06 → 2026-08-08)

**All six milestones (M0–M6) are complete.** The port ran over three
calendar days: M0 (bootstrap) and M1 (headless core extraction) on
2026-08-06; M2 through the bulk of M6 (SDL3/Metal app skeleton, the
SDL_GPU renderer, geometry/text porting, input and picking, the full
ImGui UI, persistence, packaging and CI) on 2026-08-07; the remaining
M6 tasks (release artifacts, demo video, this documentation pass) on
2026-08-08. Plan: [docs/superpowers/plans/2026-08-06-macos-metal-port.md](superpowers/plans/2026-08-06-macos-metal-port.md)
— every step is now checked off, with inline deviation notes wherever a
task's real outcome (recorded here, task by task, as it happened)
diverged from the plan's original sketch. The two largest such
deviations: Milestone 6's task list grew a `linux-sdl` CI job and a
`release` job that the plan's Task 6.2 sketch didn't anticipate (folded
into Task 6.3 as the release work landed), and Task 5.3 turned up a
real pre-existing bug — `lib/nvstore.c`, the settings-persistence
backend both frontends share, was a complete no-op stub upstream —
fixed as part of this port rather than deferred, since Task 5.3's own
persistence requirement was unimplementable otherwise.

- Plan: [docs/superpowers/plans/2026-08-06-macos-metal-port.md](superpowers/plans/2026-08-06-macos-metal-port.md)
- Upstream: https://github.com/jabl/fsv (tracked on `master`)
- Status: **M0–M6 complete.** M1 (headless core) done — M2 (dependencies + app skeleton) done — M3 done — M4 (input + picking) done — M5 done (UI parity + persistence) — M6 done (Xcode project, CI, release artifacts, demo video, this documentation pass)

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

## Task 2.2 verification (SDL3 + Metal app skeleton, ImGui, animation tick)

`src/sdl/main.cpp` is a real, compiling SDL3 + SDL_GPU + ImGui frontend:
opens a resizable HiDPI window titled "fsv", creates a GPU device
requesting MSL + SPIR-V shader formats, claims the window, installs all
five `fsv_platform` hooks, initializes ImGui's SDL3 + SDLGPU3 backends,
then loops (poll → forward to ImGui → `fsv_animation_tick()` → render
`ImGui::ShowDemoWindow()` over a `{0.08,0.10,0.12,1.0}` clear → submit),
idle-waiting via `SDL_WaitEventTimeout` when nothing animates. No scene
rendering yet — `sdl_render_frame()` is a deliberate no-op until M3.

Identifiers were cross-checked against the vendored example
(`subprojects/imgui/example_sdl3_sdlgpu3_main.cpp.txt`,
v1.92.9b-docking) rather than the brief's code block; no drift was
found beyond the brief's already-intentional simplifications (no
multi-viewport support, no `SwapchainComposition`/`PresentMode` fields
set on `ImGui_ImplSDLGPU3_InitInfo` since both only matter in
multi-viewport mode and their struct defaults — `SDR`/`VSYNC` — already
match the vendored example's explicit values).

**Meson wiring:** the root `if frontend == 'sdl' and sdl3dep.found()`
arm (Task 2.1) now also calls `add_languages('cpp', required: true)`
before `subproject('imgui')` — the root project only declares `'c'`,
so C++ has to be added explicitly before *this* project (as opposed to
the imgui subproject, which brings its own compiler) can compile a
`.cpp` file. A **second**, separate `if frontend == 'sdl' and
sdl3dep.found()` block was added right after `subdir('src')` (not
folded into the first one) to call `subdir('src/sdl')` — it has to run
after `subdir('src')` because `src/sdl/meson.build` consumes
`libfsvcore`/`incdir`/`libmisc_dep`/`libdebug_dep`, all defined inside
`subdir('src')`. The GTK arm (`src/meson.build`) is untouched.

**Headless stubs, temporarily:** `fsv_animation_tick()` reads
`globals.need_redraw` (defined in `common.o`), and `common.o` itself
calls `gui_update()` at line 462 (keeping the GUI responsive while
shelling out to `file`) — so linking in `common.o` (pulled in
transitively via `libfsvcore`) requires `gui_update` to resolve even
though this task never calls `scanfs`/`colexp` itself. `src/sdl/
meson.build` links `tools/fsv-headless-stubs.c` directly (via a
relative path, since `subdir('tools')` runs *after* `subdir('src')`
and its `headless_stubs_src` variable doesn't exist yet at this point)
as the smallest correct choice — those are exactly the 30 stub symbols
this executable needs and already exist for this purpose. Documented
in a comment at the top of `src/sdl/meson.build`; will be replaced
task by task (M3 for geometry/viewport, M5 for dirtree/filelist) as
real UI panels land.

**Build:** `meson setup builddir-sdl -Dfrontend=sdl && ninja -C
builddir-sdl` — clean, 28 targets, zero warnings from `main.cpp` or
the headless-stubs compile unit (the pre-existing `G_LOG_DOMAIN`
redefinition warning in `fsv-scan.c`/`test_scanfs.c` is unrelated,
already present before this task).

**Run, GPU driver:** `./builddir-sdl/src/sdl/fsv` logs (via Apple's
unified logging, macOS 26.6, Apple Silicon):
```
Metal API Validation Enabled
GPU driver: metal
```

**Visual verification:** `screencapture -x` is unavailable in this
sandbox — it fails with `could not create image from display` even on
a bare desktop with no app running (a Screen Recording TCC permission
the sandboxed shell doesn't have, not an app bug). Per the task's own
fallback ("use SDL's own capabilities"), a **temporary** debug-only
capture path was added to `main.cpp`, gated behind an
`FSV_DEBUG_SCREENSHOT_BMP` env var: on frame 30 it re-renders the same
`ImDrawData` into an ordinary offscreen `SDL_GPUTexture` (the real
swapchain texture is `framebufferOnly` on Metal and rejects
`SDL_DownloadFromGPUTexture` as a copy source — confirmed by hitting
exactly that Metal validation assertion first), downloads it via a
`SDL_GPUTransferBuffer`, and writes the raw bytes to disk. (First
attempt wrote a BMP via `SDL_CreateSurfaceFrom`/`SDL_SaveBMP`; the
32-bit V4-header BMP round-tripped through both Pillow and `sips` with
channels shuffled — a decoder-side artifact, reproduced and diagnosed
by reading raw pixel values with PIL — so the final version writes a
trivial P6 PPM directly from the mapped buffer instead, using the
already-logged, confirmed-`B8G8R8A8_UNORM` swapchain format to pick
the right byte order.) This confirmed a dark teal background
(`(26,20,255,31)`→ decoded correctly once the channel order matched:
≈(20,26,31,255), i.e. `{0.08,0.10,0.12,1.0}`) with the ImGui Demo
window rendered on top, dark theme, fully legible. **This capture code
was fully reverted before committing** — `main.cpp` as committed has
no trace of it (`grep -n "DEBUG_SCREENSHOT\|TEMP" src/sdl/main.cpp` →
no matches); it existed only to produce the proof screenshot at
`/tmp/fsv-t22-capture2.png`, referenced from the Task 2.2 report.

**Quit path:** sent `SIGTERM` to the running process (closing the
window/Cmd+Q wasn't automatable in this sandbox either, for the same
reason screencapture isn't) — `wait $PID` → exit code `0`, confirming
a clean shutdown path either way (SDL/macOS's default termination
handling, or explicit `SDL_EVENT_QUIT` handling in the loop — both
paths converge on the same `ImGui_ImplSDLGPU3_Shutdown` → `..SDL3_
Shutdown` → `ImGui::DestroyContext` → `SDL_ReleaseWindowFromGPUDevice`
→ `SDL_DestroyGPUDevice`/`SDL_DestroyWindow`/`SDL_Quit` → `return 0`
sequence at the bottom of `main()`).

**Idle CPU:** `SDL_WaitEventTimeout(nullptr, 16)` bounds the loop to
roughly a 16 ms tick when nothing animates, rather than a tight
unthrottled spin (which would pin a core at ~100%) — this is the exact
idiom named in the brief. Measured with `ps` while idle: **6-13%**, not
literally 0%. Two things account for that, both expected at this
stage: (1) the loop still runs a full `ImGui::NewFrame`/`Render` and
GPU submit every ~16 ms even when idle — a true zero-CPU idle would
skip rendering entirely when neither `animating` nor
`g_frame_requested` is set, which is out of scope for this task's
brief-specified mechanism; (2) `SDL_CreateGPUDevice(..., /*debug*/
true, ...)` (also brief-specified) enables Metal API Validation, which
adds meaningful per-command-buffer overhead. Not a busy-spin bug; just
worth being precise about "near zero" here rather than overclaiming.

**Self-review caught one bug before commit:** the first draft had
`ImGui_ImplSDLGPU3_Shutdown()` before `ImGui_ImplSDL3_Shutdown()`
(renderer-then-platform) — backwards from the vendored example's
platform-then-renderer order. Fixed to match the example exactly, per
this task's explicit "follow the example" instruction for shutdown
order.

**Regression checks:** `meson test -C builddir-sdl scanfs` → `1/1
fsv:scanfs OK`. Default `meson setup builddir-gtk-check` (no
`-Dfrontend`) still configures 8 targets with no SDL/imgui subproject
involvement and `ninja -C builddir-gtk-check` builds cleanly
end-to-end (both scratch dirs deleted after verification, per the
existing per-task convention).

## Task 3.1 verification (shader port + offline compilation)

Ported `src/fsv-vertex.glsl`, `src/fsv-fragment.glsl`,
`src/fsv-text-vertex.glsl`, `src/fsv-text-fragment.glsl` (OpenGL 3.1 /
GLSL 140) to Vulkan-flavored GLSL 4.50 at `shaders/src/scene.vert`,
`scene.frag`, `text.vert`, `text.frag`. `src/*.glsl` are untouched (the
GTK frontend still loads them via gresource); `src/fsv-about-*.glsl`
were skipped per the brief (about splash dropped in this port).

**Toolchain chosen: glslangValidator + spirv-cross, not
`sdl3_shadercross`.** `brew search shadercross` only turns up the
unrelated `shaderc` — no `sdl3_shadercross`/`shadercross` formula
exists in Homebrew core as of this task. Installed `glslang` (GLSL450
→ SPIR-V, `glslangValidator -V`) and `spirv-cross` (SPIR-V → MSL,
`spirv-cross --msl`) instead — the brief's own second-choice path,
confirmed to work end to end. `spirv-tools` (for `spirv-val`) was
installed alongside for validation. `tools/compile-shaders.sh` encodes
the full pipeline (`set -euo pipefail`, loop over
`shaders/src/*.vert *.frag`, validates each `.spv` with `spirv-val` if
present, emits `shaders/compiled/<name>.<stage>.{spv,msl}`).

**Entry points (confirmed against
`subprojects/imgui/backends/imgui_impl_sdlgpu3.cpp:475-492`, which
hard-codes exactly this split for its own precompiled shaders):**
SPIR-V shaders keep entry point `"main"`; MSL shaders compiled via
SPIRV-Cross get renamed to `"main0"` (Metal reserves `main`). Task 3.2
must pass the matching `entrypoint` string per `SDL_GPUShaderFormat`
when calling `SDL_CreateGPUShader`.

**Uniform inventory derived from `src/ogl.c` / `src/geometry.c` /
`src/tmaptext.c` (reality), not the brief's sketch:**

| Source | Uniform | GL call site | Frequency |
|---|---|---|---|
| scene vertex | `mvp`, `modelview`, `normal_matrix` | `ogl_upload_matrices()` (ogl.c:335-338) | once/frame |
| scene vertex+frag | `light_pos` | `glUniform4fv` at light setup (ogl.c:197) | once at startup |
| scene vertex+frag | `lightning_enabled` | `ogl_enable_lightning`/`disable_lightning` (ogl.c:349-359, geometry.c:101,114,142,181) | **per draw call**, many times/frame |
| scene fragment | `color` | `glUniform4f`/`glUniform4fv` (geometry.c:117,141,180,2792,2803) | **per draw call** |
| scene fragment | `ambient`, `diffuse`, `specular` | `glUniform1f` at light setup (ogl.c:194-196) | once at startup |
| text vertex | `mvp` | `text_upload_mvp()` (tmaptext.c:511-514), called once/frame after `ogl_upload_matrices()` | once/frame |
| text fragment | `color` | `glUniform3f` (tmaptext.c:504) | per label |
| text fragment | `tex` | `glUniform1i(glt.texture_location, 0)` (tmaptext.c:329) | once at startup |

**Deviation from the brief's sketch:** the brief's example vertex UBO
only listed `mvp`, `modelview`, `normal_matrix` — it omitted
`light_pos` and `lightning_enabled`, both of which the *original*
`fsv-vertex.glsl` actually reads (`if (lightning_enabled) { lightPos =
modelview * light_pos; ... }`). Both are included in
`SceneVertUBO` in the ported shader; dropping them would have silently
broken the lighting toggle at the vertex stage. `lightning_enabled` is
necessarily duplicated into both the vertex UBO (set=1 binding=0) and
the fragment UBO (set=3 binding=0): the original GLSL 140 shaders
shared one uniform namespace per `glProgram` across both stages, but
SDL_GPU's per-stage UBOs don't, so the same CPU-side value must be
pushed twice.

**std140 layout choice:** every uniform block uses an explicit
`layout(std140, set=N, binding=N)` qualifier rather than relying on
glslang's implicit default (confirmed to already resolve to std140 by
recompiling before/after adding the qualifier and diffing the
resulting `.spv`/`.msl` byte-for-byte — identical either way, but the
explicit qualifier documents the contract instead of depending on a
compiler default). `normal_matrix` is declared as `mat4` (not `mat3`)
in `SceneVertUBO`, per the brief's own instruction. std140
packs a `mat3` as three vec4-aligned columns with undefined trailing
padding that's easy to get wrong on the C++ side; declaring it `mat4`
(upper-left 3×3 is the real data, last row/column are inert) and
truncating with `mat3(u.normal_matrix)` in-shader removes the ambiguity
— the C++ UBO struct in `gpu.h` (Task 3.2) must mirror the full `mat4`,
not a `mat3`.

**Semantic diff summary (every line of lighting/color math accounted
for):**
- `scene.vert`: identical control flow and math to `fsv-vertex.glsl`
  line-for-line (`gl_Position`, the `lightning_enabled` guard,
  `lightPos`, `fragPos = fragTmp.xyz/fragTmp.w`, `fragNormal =
  normal_matrix * normal`) — only the uniform/attribute/varying syntax
  changed (`uniform`→UBO member, `in`/`out`→`layout(location=N)`).
- `scene.frag`: identical early-return, ambient/diffuse/specular/
  Blinn-esque specular computation, and final
  `outputColor` composition. The only line *not* carried over is the
  original's commented-out debug visualization
  (`//outputColor = 0.9999 * vec4(abs(fragNN), 1.0) + ...`) — it was
  inert dead code in the original (never compiled) and is called out
  explicitly in a comment in `scene.frag` rather than silently dropped.
- `text.vert`: identical (`gl_Position = mvp * vec4(position, 1.0);
  Texcoord = texcoord;`).
- `text.frag`: identical (`texture(tex, Texcoord)`, `vec4(color,
  alpha.r)`).

**Verification:**
- `tools/compile-shaders.sh` run twice in a row; `diff -rq` between the
  two runs' `shaders/compiled/` output reported **no differences** —
  SPIR-V carries no build timestamp and SPIRV-Cross's MSL text output
  is a pure function of its input, so the pipeline is deterministic
  with no extra flags needed.
- `spirv-val` passed on all 4 `.spv` modules
  (`scene.vert.spv`, `scene.frag.spv`, `text.vert.spv`,
  `text.frag.spv`).
- `xcrun -sdk macosx metal --version` fails on this machine
  (`cannot execute tool 'metal' due to missing Metal Toolchain`) — no
  full Metal Toolchain is installed here, and downloading one
  (`xcodebuild -downloadComponent MetalToolchain`) is a large,
  license-gated download out of scope for this task to trigger
  unattended. Per the brief's own fallback, this is **noted rather
  than worked around**: MSL correctness instead rests on (a) manual
  inspection of the SPIRV-Cross output (recorded above — struct
  layouts, buffer/texture/sampler indices, and control flow all match
  the GLSL source line-for-line) and (b) Task 3.2's runtime
  `SDL_CreateGPUShader` call, which will fail loudly if any `.msl` is
  malformed.
- Byte sizes: `scene.vert.msl` 1011 B, `scene.frag.msl` 1488 B,
  `text.vert.msl` 531 B, `text.frag.msl` 541 B; `.spv` modules
  1.0-3.2 KB. All 8 artifacts present under `shaders/compiled/`.

## Task 3.2 verification (SDL_GPU renderer core)

`src/sdl/gpu.h` + `src/sdl/gpu.cpp` replace `src/ogl.c` for the SDL
frontend: GPU device, scene pipelines, the `FsvMesh` buffer API, the
camera matrices and the per-frame scene pass. `gpu.h` is a pure C
header so `geometry.c` can call it directly in Task 3.3;
`src/sdl/gpu_internal.hpp` holds the few SDL-typed entry points
`main.cpp` needs (device + per-frame command buffer/swapchain).

**MSL works on Metal.** This was the first time the Task 3.1 `.msl`
artifacts met a real Metal compiler (`xcrun metal` is unavailable on
this machine, so they had never been compiled at all). Both stages
loaded on the first try:

```
gpu: driver metal, shader formats 0x30   (MSL | METALLIB)
gpu: loaded scene.vert.msl (1011 bytes, entry "main0")
gpu: loaded scene.frag.msl (1488 bytes, entry "main0")
gpu: scene + id pipelines created
```

No patching of the SPIRV-Cross output was needed, and the entry-point
split Task 3.1 predicted (`main0` for MSL, `main` for SPIR-V) is
correct. The loader still tries formats in order — MSL, then SPIR-V —
and skips any the device does not advertise via
`SDL_GetGPUShaderFormats()`, so a Vulkan/Linux build can use the same
code path with the `.spv` blobs.

Shaders are **embedded in the binary** as byte arrays generated by
`tools/embed-shaders.py` at build time (a meson `custom_target`),
rather than loaded from disk: the frontend has to work from a build
tree, an install prefix and (from Task 6.1) an `.app` bundle, and a
runtime path lookup is three ways to fail at startup for no benefit.
This is what the vendored ImGui SDLGPU3 backend does with its own
shaders.

**Depth format: `D32_FLOAT`.** `D24_UNORM` is checked first (per the
brief) but `SDL_GPUTextureSupportsFormat()` reports it unsupported on
this Apple Silicon GPU; `D32_FLOAT` is the fallback and the one in
use. The depth texture is (re)created whenever the acquired swapchain
image changes size, so no window-resize event handling is needed.

**Pass structure: two passes per frame**, both on the swapchain
texture — scene (`LOADOP_CLEAR`, depth attached) then ImGui
(`LOADOP_LOAD`, no depth). One pass would have been marginally
cheaper, but ImGui must not be depth-tested and must not inherit or
leak viewport/scissor state (its backend warns about exactly that at
`imgui_impl_sdlgpu3.cpp:312`). Ending the scene pass gives complete
state isolation for one extra Metal encoder.

**Clip space: no Y flip, but zero-to-one depth.** `SDL_gpu.h`'s
"Coordinate System" section pins SDL_GPU to the D3D12/Metal
convention: NDC lower-left `(-1,-1)`, upper-right `(1,1)` — the same Y
orientation OpenGL uses — with Z in `[0,1]` instead of `[-1,1]`. So
the ported shaders need no flip (confirmed against the vendored ImGui
MSL shader, which carries an explicit `gl_Position.y *= -1.0` that its
SPIR-V twin does not, precisely because ImGui authors its GLSL for
Vulkan's Y-down NDC and has to undo it), and only the projection
changes: `glm_frustum_rh_zo()` instead of `glm_frustum()`. The `_zo`
variant lives in `<cglm/clipspace/persp_rh_zo.h>`, which
`<cglm/cglm.h>` does not pull in — it is included explicitly rather
than flipping `CGLM_CLIP_CONTROL` project-wide, which would silently
retarget the still-OpenGL GTK frontend.

**Verification:**
- `ninja -C builddir-sdl` clean, zero warnings from `gpu.cpp`/
  `main.cpp`. Default (GTK) `meson setup` still configures and builds
  the headless core — the GTK arm is untouched.
- Ran headed: the log above, then a frame identical to Task 2.2's
  (clear color + ImGui demo window), now produced by `gpu.cpp`'s
  two-pass structure with the scene pipeline bound and a depth
  attachment. `screencapture` is still blocked by this shell's TCC
  restrictions (it returns an all-black image), so the frame was
  verified with the same temporary offscreen-texture readback used in
  Task 2.2 — redirect the frame into an ordinary `SDL_GPUTexture`,
  `SDL_DownloadFromGPUTexture`, write a PPM — and the scaffold was
  reverted before commit (`grep -n "CAPTURE\|g_capture" src/sdl/*.cpp`
  is empty on the committed tree).
- `meson test scanfs` → 1/1 OK. Idle CPU measured with `ps`: **0.2 –
  0.8%** (the render-on-demand loop from Task 2.2's follow-up still
  holds; nothing per-frame was added outside the render path).
- The UBO layouts are guarded by `static_assert`s on `sizeof` and on
  every member offset, so a drift between `gpu.cpp` and
  `shaders/src/scene.{vert,frag}` is a compile error rather than
  silent garbage on screen. Same for `FsvVertex`'s 40-byte stride.

### Task 3.3 handoff: what `FsvMesh` does *not* cover yet

> **Superseded by the Task 3.3 section below.** `FsvMesh` was removed
> rather than extended; the inventory in this table is still accurate as
> a description of what `geometry.c` draws, and is what Task 3.3's
> topology expansion was derived from.

`fsv_mesh_draw()` issues exactly one kind of draw —
`SDL_DrawGPUIndexedPrimitives()` on a `TRIANGLELIST` pipeline. That
covers `geometry.c`'s four `glDrawElements(GL_TRIANGLES, ...)` call
sites (lines 1009, 2037, 2183, 3051) and nothing else. Task 3.3 owns
extending the API; this is the precise inventory it has to satisfy, so
that nothing is discovered halfway through the port:

| `geometry.c` | Topology | Draw kind |
|---|---|---|
| 404 (`drawVertex`) | `GL_TRIANGLE_FAN` | non-indexed |
| 2116, 2280, 2306, 2372 (`drawVertex`) | `GL_TRIANGLE_STRIP` | non-indexed |
| 1061 (`drawVertexPos`) | `GL_LINE_LOOP` | non-indexed |
| 2245 (`drawVertexPos`), 2708 | `GL_LINE_STRIP` | non-indexed |
| 1259, 2123 (`drawVertexPos`) | `GL_LINES` | non-indexed |
| 1009, 2037, 2183, 3051 | `GL_TRIANGLES` | indexed — **covered today** |

Three consequences for Task 3.3:

1. **Non-indexed draws.** Everything routed through `drawVertex()` /
   `drawVertexPos()` (`geometry.c:143`, `:184`) is `glDrawArrays`. That
   needs `SDL_DrawGPUPrimitives()` and a mesh that can be uploaded
   without an index buffer — `fsv_mesh_upload()` currently requires both
   arrays and rejects the call otherwise.
2. **Topologies are baked into the pipeline** in SDL_GPU
   (`primitive_type` is a `SDL_GPUGraphicsPipelineCreateInfo` field, not
   a draw argument), so each of `TRIANGLELIST` / `TRIANGLESTRIP` /
   `LINELIST` / `LINESTRIP` needs its own pipeline object. Note there is
   **no `TRIANGLEFAN` and no `LINELOOP`** in SDL_GPU: line 404's fan and
   line 1061's loop have to be converted at mesh-build time (a fan
   becomes an index list; a loop becomes a strip with the first vertex
   repeated).
3. **Index width.** `geometry.c` uses `GL_UNSIGNED_SHORT` (16-bit)
   indices; `fsv_mesh_upload()` takes `const unsigned int *` per the
   task brief's contract and binds `INDEXELEMENTSIZE_32BIT`. Task 3.3
   converts on upload or widens the call sites.

Also deferred to Task 3.3: **`glPolygonOffset(1.0, 1.0)`** (enabled in
`ogl_init()`, not ported). SDL_GPU spells it `enable_depth_bias` +
`depth_bias_constant_factor`/`depth_bias_slope_factor` on the rasterizer
state. The right values are only observable once fills and their
outlines are drawn together, which is exactly when the line topologies
above arrive — if platform fills z-fight with their outlines, this is
why.

## Task 3.3 verification (geometry.c off OpenGL, scene renders via Metal)

`src/geometry.c` no longer contains a single GL call
(`grep -c "\bgl[A-Z]" src/geometry.c` -> `0`) and no longer includes
`ogl.h`, hence no epoxy. It draws against `src/gpu.h`, which each
frontend implements: `src/sdl/gpu.cpp` (SDL_GPU) and
`src/ogl-gpu-compat.c` (epoxy/GL, so `-Dfrontend=gtk` keeps working).
`gpu.h` moved from `src/sdl/` to `src/` to say so.

**The scene renders.** All three modes, on a real directory
(`./builddir-sdl/src/sdl/fsv src --mapv|--treev|--discv`):

- **MapV**: the classic landscape. A grey directory slab carrying the
  black folder outline, with yellow file pedestals laid out in rows of
  descending size, taller ones at the back; two grey sub-directory
  pedestals with their own folder outlines; side faces shaded dark
  (ambient-only, correct for a light at `(0.2, 0, 1, 0)`); the white
  node cursor bracketing the root, with its occluded corner bars
  showing through the pedestals in grey (the `GL_GREATER` pass) and its
  visible corners in white (the `GL_LEQUAL` pass).
- **TreeV**: the grey platform seen from the front, yellow leaf nodes in
  concentric rows, two grey directory leaves at the rear, the red branch
  stem descending from the platform's inner edge, cursor bracket around
  the platform.
- **DiscV**: the grey root disc ringed by yellow file discs, sized by
  file size and staggered clockwise, smallest tapering off at the
  bottom.

### Topology strategy: expand everything into two pipelines

SDL_GPU has no `TRIANGLEFAN` and no `LINELOOP`, and bakes the primitive
type into the pipeline object. Rather than carry a pipeline per GL mode,
`gpu_draw()` expands every topology into indices against exactly two:
`TRIANGLELIST` and `LINELIST`.

| `geometry.c` draws | expanded to |
|---|---|
| `GL_TRIANGLES` (indexed) | indices passed through unchanged |
| `GL_TRIANGLE_FAN` | `(0, i, i+1)` |
| `GL_TRIANGLE_STRIP` | `(i, i+1, i+2)`, first two swapped on odd `i` so winding is uniform |
| `GL_LINES` | `(i, i+1)` per pair |
| `GL_LINE_STRIP` | `(i, i+1)` |
| `GL_LINE_LOOP` | line strip plus `(n-1, 0)` |

Index expansion is a few integer appends over data that is being copied
anyway; a strip pipeline would have saved nothing, since the fan and the
loop still need converting and neither could share it. Indices are
32-bit throughout — `geometry.c` used `GL_UNSIGNED_SHORT`, and widening
on the way in was cheaper than a second index-size code path.

Pipelines are built lazily and cached by (primitive type, depth
comparison, color-target format). A normal frame uses two; the node
cursor adds two more the first time it is drawn; picking (Task 4.2) will
add the offscreen-format variants.

### Recording and replay, not retained meshes

Task 3.2's `FsvMesh` handle API was removed in favor of one immediate
call, `gpu_draw(topology, verts, nverts, indices, nindices)`. This was
not a stylistic preference. `geometry.c` owns no persistent meshes: each
call site rebuilt its vertex array and re-specified one shared streaming
VBO on every frame. Modeling that as one `FsvMesh` per call site is
wrong under SDL_GPU, because buffer copies are illegal inside a render
pass, so the uploads must be recorded before the draws that consume
them — and a mesh reused across every node in a frame then paints all of
them with the last node's geometry. Cycling only papers over it by
allocating a fresh internal buffer per node per frame.

So `gpu_draw()` records: vertices and expanded indices append to two CPU
arenas, and a `DrawCmd` captures the pipeline, the index range and a
snapshot of both uniform blocks. `gpu_scene_end()` uploads both arenas in
one copy pass and replays every `DrawCmd` in one render pass. A frame
costs one transfer and one pass regardless of node count, the uniform
snapshots preserve exactly the per-draw state the GL code expressed
through ordering, and `geometry.c` keeps its "build a little array, draw
it, move on" shape unchanged.

Consequence for Task 3.4: text drawn between `gpu_scene_begin()` and
`gpu_scene_end()` has to join the same recording (or open its own pass
afterwards with `LOADOP_LOAD` on color and depth). It cannot simply
issue draws mid-walk, for the same reason geometry cannot.

### Depth bias (`glPolygonOffset`)

`ogl_init()`'s `glEnable(GL_POLYGON_OFFSET_FILL)` + `glPolygonOffset(1.0,
1.0)` becomes `enable_depth_bias = true`,
`depth_bias_constant_factor = 1.0f`, `depth_bias_slope_factor = 1.0f`,
`depth_bias_clamp = 0.0f` — the same numbers, and the same sign
convention (positive pushes fills away from the viewer). It is set on
the **triangle** pipelines only: `GL_POLYGON_OFFSET_FILL` applies to
filled polygons, not lines, and the whole point is for the black folder
outlines and the cursor bars — which sit exactly on a face's plane — to
win the depth test. Verified visually: the folder outlines on the MapV
directory faces and the TreeV folder leaves render solid, with no
z-fighting — including at scale, where MapV on a 275,557-node tree
(`/opt/homebrew/Cellar`) draws several hundred collapsed-directory folder
outlines across the root slab, every one crisp and unbroken down to the
~6-pixel ones at the far edge. A wrong bias value shows up first as
stippled outlines in exactly that picture.

### Line width: 1 pixel, everywhere

`geometry.c` did ask for other widths — `glLineWidth(3.0)` around DiscV,
and 2.0 / 5.0 for the cursor's hidden and visible halves. SDL_GPU has no
line-width control at all (no field in `SDL_GPURasterizerState`; Metal
has no `glLineWidth` equivalent), so `gpu_set_line_width()` is a
documented no-op on the SDL backend and every line rasterizes 1 pixel
wide. **This is a visible difference from the GTK build**: the node
cursor's visible half reads as a thin white outline rather than a thick
one. The GL shim still calls `glLineWidth()`, so the GTK frontend is
unchanged. Emulating widths with quads is deliberately not done here.

### The GTK compat shim

`src/ogl-gpu-compat.c` (~220 lines) is a transcription of the blocks
deleted from `geometry.c`: same `GL_STREAM_DRAW` usage hints, same
attribute setup, same "re-specify the buffer as empty afterwards to
avoid an implicit sync" trick, one shared streaming VBO/EBO pair instead
of one `static GLuint vbo` per call site. Color and lighting are cached
and applied inside `gpu_draw()`, which is what `drawVertex()` /
`drawVertexPos()` did; depth function and line width are plain GL server
state and apply immediately.

`gpu_mat` is now the single storage for the projection/modelview pair on
**both** frontends — `ogl.c` writes it in `setup_*_matrix()`, and
`FsvGlState` lost its own copies, because two copies of a matrix
`geometry.c` pushes and pops is two sources of truth. `ogl.c` also lost
`ogl_upload_matrices()` and `ogl_{enable,disable}_lightning()`, which are
now `gpu_upload_matrices()` / `gpu_set_lighting()`. The old
`ogl_upload_matrices(gboolean text)` argument is gone: the text engine's
MVP is now always refreshed with the scene's. Every caller that passed
`FALSE` did so only to skip work, and the one place that wants a
different text matrix (`about_splash_draw()`'s ortho projection) calls
`text_upload_mvp()` directly afterwards.

`gpu_init()`/`gpu_shutdown()`/`gpu_pick()` are deliberately *not* in the
shim: device setup is `ogl_init()`, and `viewport.c` calls
`ogl_select_modern()` directly. Nothing in the GTK arm references them.

### Deviations from the task brief

1. **The splash screen and the "fsv" logo moved to `about.c`.**
   `geometry_gldraw_fsv()` and `splash_draw()` were the only drawing in
   `geometry.c` that did not use the scene shader: they use the separate
   *about* program (per-vertex color + linear fog,
   `src/fsv-about-*.glsl`), which Task 3.1 never ported and `gpu.h` has
   no pipeline for. Keeping them would have meant putting a second
   shader program into the shared contract that only the GTK frontend
   could implement. `about.c` already drew the same letters through the
   same program, so they moved there; `geometry.c`'s `FSV_SPLASH` case
   calls `about_splash_draw()`, which the SDL frontend no-ops. **The SDL
   frontend therefore has no splash screen and no About presentation**;
   reinstating them needs the about shaders ported (Task 3.1 scope).
2. **`geometry.c` is a source of each frontend target, not part of
   `libfsvcore`.** The brief expected it to join the core library, but it
   now calls `gpu.h`, and `libfsvcore`'s other consumers (`fsv-scan`,
   `test_scanfs`) have no renderer to link against. It is listed in both
   frontends' source lists instead, and the SDL target uses its own
   `src/sdl/stubs.c` rather than `tools/fsv-headless-stubs.c`, whose
   `geometry_*` no-ops would collide.
3. **`FsvMesh` was removed rather than extended** — see "Recording and
   replay" above. `fsv_mesh_draw_id()` was already dropped in Task 3.2
   and stays dropped; picking goes through `node_set_color()`'s id
   colors plus `gpu_set_color()` / `gpu_set_lighting(0)`, with
   `gpu_render_mode()` replacing `gl.render_mode`.
4. **`FsvVertex` lost its per-vertex `color[4]`** (40 -> 24 byte stride,
   two vertex attributes). `scene.vert` declares only `position` and
   `normal`; the third attribute was never read.
5. **Default mode is MapV, not DiscV.** The brief's goal state says
   "DiscV landscape (default mode)", but `src/fsv.c`'s default is
   `FSV_MAPV` and MapV is the mode with the pedestal landscape the same
   sentence describes. Matching `fsv.c` seemed more defensible than
   diverging from it; `--discv` / `--mapv` / `--treev` select the mode
   until Task 5.1's menu bar exists.
6. **One behavior fix**: `cursor_post()` now restores the depth function
   to `LESS`. The GL original never restored it, so every draw after the
   first node cursor ran with `GL_LEQUAL` for the rest of the session.

### `--screenshot FILE`

Renders one frame into an offscreen `R8G8B8A8_UNORM` texture
(`gpu_screenshot_begin()` / `gpu_screenshot_end()` in `gpu.cpp`),
downloads it with `SDL_DownloadFromGPUTexture()` behind a fence, and
writes it with `SDL_SaveBMP()`. It first lets the intro camera pan
finish — morphs advance on wall-clock time, so it has to actually wait —
bounded at 8 seconds so a stuck animation cannot hang CI. The scene path
is identical to the windowed one: same recording, same pipelines, only
the color-target texture and its format differ.

All three screenshots above were produced this way and are non-black.
`screencapture` and AppleScript window queries remain blocked by this
shell's TCC restrictions (same as Task 3.2), so the offscreen readback
is how the frame was inspected; the windowed path was verified to run,
render and stay at ~0.9% CPU.

### Verification

- `grep -c "\bgl[A-Z]" src/geometry.c` -> `0`; `grep -n "epoxy\|ogl" src/geometry.c` -> empty.
- **macOS / SDL**: `meson setup -Dfrontend=sdl` + `ninja` clean from
  scratch, no warnings from any file touched here (the two
  `G_LOG_DOMAIN` redefinition warnings are pre-existing, in
  `fsv-scan.c` and `test_scanfs.c`).
- **Linux / GTK** (Debian bookworm container): `meson setup` + `ninja`
  clean from scratch, 11 targets, zero warnings. `src/fsv` links
  `ogl-gpu-compat.c.o` and exports `gpu_draw`, `gpu_set_color`,
  `gpu_set_lighting`, `gpu_set_depth_test`, `gpu_set_line_width`,
  `gpu_render_mode`, `gpu_upload_matrices`. A *headed* GTK run is not
  possible in the container, so the GTK arm is verified to build and
  link, not to render.
- `meson test scanfs` -> `1/1 OK` on both arms.
- Idle CPU after the intro animation settles: **0.7 - 1.0%** measured
  with `ps` on a 275,557-node tree (`/opt/homebrew/Cellar`), RSS 133 MB.
  A 747-node tree screenshots end to end in 4.3 s, of which 4.0 s is the
  intro pan it waits out.
- `-DDEBUG` now applies to C++ as well as C. `common.h` routes the
  `GList` helpers through `debug.c`'s tracking allocator under `DEBUG`,
  and with the flag on only one side a link prepended by `main.cpp` was
  untracked when `camera.c` removed it ("Attempted to free unknown
  link" at startup).

## Task 3.4 verification (3D text labels via SDL_GPU)

`src/tmaptext.c` no longer contains a single GL call
(`grep -c "\bgl[A-Z]" src/tmaptext.c` -> `0`, and it no longer includes
`ogl.h`) and no longer includes `<gio/gio.h>`. Directory and file name
labels render on both frontends.

### Route chosen: ported tmaptext.c in place, not a new text3d.cpp

The task brief offered a choice: port `tmaptext.c` in place behind a small
addition to `gpu.h` (mirroring Task 3.3's `geometry.c` split), or create
`src/sdl/text3d.cpp` per the plan's original file map. **In place**, for
the same reason Task 3.3 kept `geometry.c` as one file instead of moving
it into a frontend directory: `tmaptext.c`'s actual GL surface is small
(55 calls, all texture upload, one shader program, and one VBO/EBO draw)
and everything else -- `xbm_pixels()`, `get_char_dims()`,
`get_char_tex_coords()`, and the three `text_draw_*()` layout functions --
is pure math shared verbatim by both frontends. A `text3d.cpp` would have
had to either duplicate that math into a second file (churn, and a new
place for the two copies to drift) or `#include` `tmaptext.c` from it
(worse). Six new `gpu.h` entry points -- `gpu_text_init()`,
`gpu_text_begin()`/`gpu_text_end()`, `gpu_text_draw()`,
`gpu_text_set_color()`, `gpu_text_upload_mvp()` -- get the same result
with zero duplicated math, implemented once in `src/sdl/gpu.cpp`
(SDL_GPU) and once in `src/ogl-gpu-compat.c` (epoxy/GL, so `-Dfrontend=gtk`
keeps its exact old rendering). `src/sdl/meson.build` now lists
`../tmaptext.c` as a compiled source next to `../geometry.c`, and the
matching `text_*()` no-ops were deleted from `src/sdl/stubs.c`.

`FsvTextVertex` (`{ pos[3], texcoord[2] }`) is `gpu.h`'s renamed version
of tmaptext.c's old file-local `TextVertex` struct -- moved because it is
now a cross-file contract, not because its layout changed.

### Text pipeline state (src/sdl/gpu.cpp's `text_pipeline_for()`)

| State | Value | Why |
|---|---|---|
| Blend | `SRC_ALPHA` / `ONE_MINUS_SRC_ALPHA`, same factors for color and alpha | `ogl_init()`'s one `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)` call; the GL original never calls `glBlendFuncSeparate` |
| Depth test | ON, `LESS` | Same as the scene's default; `text_pre()`/`text_post()` never touch `glDepthFunc` |
| Depth **write** | **ON** | See below -- deliberately not the "typical" text-over-geometry choice |
| Depth bias | never enabled | `GL_POLYGON_OFFSET_FILL` applies to *filled scene* polygons only, and `text_pre()` explicitly `glDisable`s it around every text draw |
| Cull mode | `BACK`, CCW front face | Same as the scene pipeline; `GL_CULL_FACE` is global GL state that `text_pre()`/`text_post()` never touched, so text was always subject to it too |

**Depth write is ON, not OFF, and that is a deliberate deviation from the
brief's own "typical" pattern for text-over-geometry.** The brief asked to
check the old GL code's `glDepthMask` state around text and match it
exactly: `grep -n "glDepthMask" src/*.c` returns nothing at all, anywhere
in the codebase. The old renderer never toggled the depth mask, meaning
text always drew with GL's default (`GL_TRUE`, write enabled) -- the same
as scene geometry. Matching that exactly, rather than switching to the
"write off" idiom that is more typical for alpha-blended text overlays,
is what `enable_depth_write = true` on the text pipeline does. This was
verified not to matter visually at the scale this renderer draws labels
at (one pass, few hundred quads, each drawn once) -- see "Recording and
replay" below for why depth *write* order is a non-issue here regardless.

### Recording and replay: text joins the same pass as geometry

Task 3.3's handoff note called this out in advance: text drawn between
`gpu_scene_begin()` and `gpu_scene_end()` has to join the same recording,
because SDL_GPU forbids buffer copies inside a render pass and
`geometry.c`'s `text_pre()`/`text_draw_*()`/`text_post()` calls are
interleaved with `gpu_draw()` mid-tree-walk, exactly like the scene
geometry itself.

The scene's `DrawCmd` recording (vertices/indices/uniform snapshot per
draw, replayed in one pass at `gpu_scene_end()`) already solved this for
geometry; text needed the same shape but not the same arena, because its
vertex format (`FsvTextVertex`: position + texcoord, no normal) and
pipeline (alpha-blended, textured, its own uniform blocks) are entirely
different from the scene's. So `gpu.cpp` carries a **second**, parallel
recording: `g_text_vertices` / `g_text_indices` / `g_text_draws`, reset in
`gpu_scene_begin()` alongside the scene's, uploaded by its own
`upload_frame_text_geometry()` (a second copy pass, same command buffer,
its own transfer buffer -- kept independent because label data is tiny
and coupling its capacity growth to the scene arena's would buy nothing),
and replayed by `replay_text_draws()` **after** `replay_draws()` inside
the *same* render pass (SDL_GPU allows rebinding vertex/index buffers and
pipelines mid-pass).

Crucially, `gpu_text_draw()` snapshots the current mvp and color into its
`TextDrawCmd` **at call time**, exactly like `gpu_draw()` does for the
scene, not once per frame: `treev_draw_recursive()` /
`mapv_draw_recursive()` (`src/geometry.c`) call `gpu_upload_matrices()`
(hence `text_upload_mvp()`) and `text_set_color()` afresh at every
directory node while re-walking the tree for its label pass, so a leaf
label's mvp is genuinely different from its parent platform's. A single
frame-wide uniform push would have painted every label with the last
node's transform and color -- this was caught by reading
`treev_draw_recursive()` before writing any recording code, not by
debugging a wrong screenshot.

Drawing all scene geometry before any text (rather than interleaving them
in tree-walk order, which the immediate-mode GL original effectively did)
is a small, intentional semantic improvement, not a regression: depth
*testing* only cares about the final state of the depth buffer at the
moment a fragment is tested, and by the time text replays, the depth
buffer already holds the true nearest-surface depth from every node --
so a label is correctly hidden behind whichever geometry is actually
closest, regardless of which was recorded first. The old interleaved GL
order could in principle show the opposite artifact (a label surviving in
front of a nearer node drawn later in the same frame); this was not
observed in testing but the recording order removes the possibility
either way.

`gpu_text_begin()`/`gpu_text_end()` (the `text_pre()`/`text_post()`
backends) are no-ops on SDL_GPU: the GL original's toggles around every
text batch (`glDisable(GL_POLYGON_OFFSET_FILL)`, `glEnable(GL_BLEND)`,
bind/unbind the atlas texture) are all either baked into the text
pipeline (blend, absent depth bias) or bound per-draw
(`replay_text_draws()`'s `SDL_BindGPUFragmentSamplers()`). The GL compat
shim (`src/ogl-gpu-compat.c`) still does the exact old GL state dance,
because that backend has no per-draw sampler binding to substitute it
with.

### Atlas format and sampler

The glyph atlas (`xbm_pixels()`'s single-channel bitmap, unchanged) uploads
as `SDL_GPU_TEXTUREFORMAT_R8_UNORM` -- the direct SDL_GPU equivalent of
the old code's `GL_RED` texture, sampled as `.r` and used as alpha by
`text.frag` (ported in Task 3.1; already did exactly this). Upload is a
one-off transfer-buffer + copy pass in `gpu_text_init()`, on its own
command buffer, since the atlas never changes after startup and does not
belong in the per-frame recording.

The sampler is **linear filtering, clamp-to-edge, no mipmaps** -- simpler
than the old GL sampler, which mixed `GL_LINEAR_MIPMAP_LINEAR`
minification with `GL_NEAREST` magnification and called
`glGenerateMipmap()`. With one mip level (the atlas is small and static,
and mipmapping a font atlas mainly helps distant minification, which this
renderer does not need enough to justify the extra levels), there is no
minification filter left to choose between, so `LINEAR`/`LINEAR` is both
simpler and a fair reading of the brief's "linear filtering,
clamp-to-edge" instruction. The GL compat shim (`src/ogl-gpu-compat.c`)
keeps the *exact* old GL sampler parameters unchanged, including the now
purely decorative `GL_TEXTURE_BORDER_COLOR` call (dead even in the
original, since the wrap mode is `GL_CLAMP_TO_EDGE`, never
`GL_CLAMP_TO_BORDER`) -- preserving old GTK behavior byte-for-byte was
the explicit mandate for that arm.

### Bug caught during verification: missing sampler count on the shader

The first build ran clean and recorded draws correctly (confirmed with
temporary logging: real vertex data, non-zero mvp, correct black label
color), but produced **zero visible pixels** -- not wrong-colored ones,
none at all, even with culling and the depth test forced off for
isolation. The cause: `create_shader()` (written in Task 3.2 for the
scene shaders, which use no samplers) hard-coded
`SDL_GPUShaderCreateInfo.num_samplers = 0` for every shader. `text.frag`
declares one `sampler2D` (set=2, binding=0, the glyph atlas) --
SDL_GPU/Metal needs that resource count to match what the shader actually
binds, and the mismatch failed silently from this renderer's point of
view (no error on stdout; Metal validation output goes through unified
logging, not the captured stream). Fixed by giving `create_shader()` an
optional `num_samplers` parameter (defaulted to 0, so the two scene call
sites are untouched) and passing 1 for `text.frag`'s shader. This is the
kind of bug the task's "visual proof mandatory" bar exists to catch: the
build, the recording, and the draw-call bookkeeping were all correct, and
only an actual rendered frame revealed the problem.

### Verification

- `grep -c "\bgl[A-Z]" src/tmaptext.c` -> `0`; no `epoxy`/`ogl.h` includes
  outside `src/ogl-gpu-compat.c`.
- **macOS / SDL**: clean `meson setup -Dfrontend=sdl` + `ninja` from
  scratch, zero warnings from any file touched in this task (the two
  pre-existing `G_LOG_DOMAIN` warnings in `fsv-scan.c`/`test_scanfs.c` are
  unrelated). `meson test scanfs` -> `1/1 OK`.
- **Linux / GTK** (Debian bookworm container): clean `meson setup` +
  `ninja` from scratch, all 11 targets including `src/fsv`, zero warnings.
  `nm` on the linked binary confirms `gpu_text_init`/`_begin`/`_end`/
  `_draw`/`_set_color`/`_upload_mvp` and `text_init`/`_draw_straight`/
  `_draw_straight_rotated`/`_draw_curved` all resolve; `text_init_shaders`
  (the relocated program-linking helper) is file-local (`t`), as intended.
  `meson test scanfs` -> `1/1 OK`. A headed GTK run is still not possible
  in the container (no display), so this arm is verified to build and
  link, exactly as in Task 3.3.
- **Visual proof (`--screenshot`, offscreen readback, same mechanism as
  Tasks 3.2/3.3)**: ran against this repo's own `src/` directory.
  - **MapV**: every pedestal and directory face carries its name in black,
    legible text sitting flush on the top face -- `geometry.c`,
    `camera.c`, `gui.c`, `dialog.c`, `xmaps`, `sdl`, and the smaller
    files' names squeezed to fit their narrower tops (`get_char_dims()`'s
    horizontal squeeze, working as before). No boxes around the glyphs,
    no z-fighting with the pedestal outlines or the folder-icon lines.
  - **TreeV**: leaf node names in black on every small leaf platform,
    correctly curved/rotated per `text_draw_straight_rotated()`; the root
    platform's own name (`src`) renders large and legible, curved along
    its inner edge in white via `text_draw_curved()`, over the red branch
    stem, alpha-blended cleanly with no artifacts.
  - **`tests/fixture`**: the narrow `file1.txt` pedestal shows its
    (heavily squeezed) label, confirming the fixture path renders text
    too, not just large real-world trees.
  - Screenshots taken before *and* after reverting the temporary debug
    instrumentation used to isolate the sampler-count bug; both are
    pixel-identical in content (culling/depth-test-always was a debug aid
    that turned out to be unnecessary once the real bug was fixed --
    culling and the normal `LESS` depth test were never the problem).
- `meson test scanfs` -> `1/1 OK` on both arms (repeated after the
  from-scratch rebuilds above). Idle CPU on a real 2600-ish-node tree
  (`src`, windowed, post intro-pan): **0.7%** (`ps`), matching Task 3.3's
  baseline -- text rendering adds one extra copy pass and one pipeline
  bind per frame, not a busy loop.

### Deviation from the task brief

The brief's file map suggested `src/sdl/text3d.cpp`; this task created no
such file (see "Route chosen" above) and instead extended `gpu.h` and
`src/ogl-gpu-compat.c`/`src/sdl/gpu.cpp`, plus `src/sdl/meson.build`
(added `../tmaptext.c` as a compiled source, and the four `text.*.msl`/
`text.*.spv` artifacts to `embed-shaders.py`'s input list) and
`src/sdl/stubs.c` (removed the now-superseded `text_*()` no-ops).

## Task 4.1 verification (input.cpp — mouse navigation and selection)

`src/sdl/input.h` + `src/sdl/input.cpp` port `src/viewport.c`'s
`viewport_cb()` (GTK's `GdkEvent` switch) onto SDL3's `SDL_Event`.
`src/sdl/main.cpp` forwards every polled event to
`input_handle_event()`, right after `ImGui_ImplSDL3_ProcessEvent()`.
`viewport.c` itself is untouched; the GTK arm's build is unaffected
(`grep -n "viewport.c" src/meson.build` still lists only the GTK
target).

### What viewport.c actually does (read before writing a line)

The brief's gesture list (middle-drag "fly" with Shift for vertical,
scroll-wheel dolly, double-click "activate") does not match this fork's
actual `src/viewport.c`, confirmed by reading it in full plus
`doc/mouse.html` (the shipped man-page-equivalent):

- **Middle-button drag dollies** the camera (`camera_dolly()`), scaled
  by `MOUSE_SENSITIVITY` (0.5) times the vertical pixel delta since the
  last motion event. There is no "flight" and no Shift modifier
  anywhere in the file.
- **Ctrl+left-button drag revolves** the camera (`camera_revolve()`),
  same sensitivity, both axes.
- **No scroll-wheel gesture exists at all.** `doc/mouse.html` names the
  middle-button drag as the viewport's *only* dolly input.
- **Double-click is explicitly a no-op**: `GDK_2BUTTON_PRESS` is `/*
  Ignore second click of a double-click */ break;`. The ordinary
  `GDK_BUTTON_PRESS` for that same second click already ran normally.
  There is no "activate"/warp/open-file action in the 3D viewport
  itself — that exists in `dirtree.c`'s separate directory-tree pane
  (Task 5.2's ImGui panel), not here.
- **Left click selects + `camera_look_at()`s** the node under the
  cursor (press picks and highlights; release, if still over the same
  node and the camera isn't already panning, flies to it).
- **Right click** brings up `context_menu()` (a GTK popup) for the
  node under the cursor, plus `filelist_show_entry()`.
- Motion-driven only: every gesture above fires from
  `GDK_MOTION_NOTIFY`'s own delta. There is no timer and no busy
  animation-loop tick that moves the camera while a button is held but
  the mouse is stationary — so `input.cpp` declares no per-frame tick
  function; `input_handle_event()` is the complete port of the
  mechanism.

This resolved the brief's own "NEEDS_CONTEXT if viewport.c's gesture
math depends on GTK specifics without an SDL equivalent" clause: it
doesn't — the math (`MOUSE_SENSITIVITY * pixel delta` fed straight into
`camera_dolly()`/`camera_revolve()`) is GTK-independent. What differs is
plumbing around it (coordinate scaling, modifier-state queries, pointer
capture), each with a direct SDL equivalent, below.

### Gesture mapping (GTK → SDL)

| Gesture | viewport.c (GTK) | input.cpp (SDL) | Notes |
|---|---|---|---|
| Dolly | Middle-button drag, `GDK_BUTTON2_MASK` in `GDK_MOTION_NOTIFY` | Middle-button drag, `SDL_BUTTON_MMASK` in `SDL_EVENT_MOUSE_MOTION` | Identical math: `camera_dolly(-(0.5 * dy))` |
| Revolve | Ctrl+left drag, `GDK_CONTROL_MASK` + `GDK_BUTTON1_MASK` | Ctrl+left drag, `SDL_GetModState() & SDL_KMOD_CTRL` + `SDL_BUTTON_LMASK` | Identical math: `camera_revolve(0.5*dx, 0.5*dy)` |
| Select + fly-to | Left click (press picks/highlights, release flies) | Same, split across `SDL_EVENT_MOUSE_BUTTON_DOWN`/`_UP` | Pick routes through `gpu_pick()` (Task 4.2 stub) |
| Context menu | Right click → `context_menu()` (GTK popup) | Right click → logged, `filelist_show_entry()` called for real | Inert until Task 5.1's ImGui menu |
| Double-click | Explicit no-op (`GDK_2BUTTON_PRESS: break;`) | No branch on `ev->button.clicks` at Task 4.1 time; **superseded** — a post-port addition now toggles expand/collapse for a directory (see "Post-port additions" below) | Task 4.1 itself reproduced the no-op by construction — SDL had no extra event to ignore. The later addition is a deliberate, labeled deviation from that parity, not a silent drift back into it |
| Scroll wheel | **Does not exist** | `SDL_EVENT_MOUSE_WHEEL` → `camera_dolly()` | **Addition**, not a port — brief and verification bar ask for it explicitly |
| Cursor icon | `GDK_DOUBLE_ARROW`/`GDK_FLEUR` swap during dolly/revolve, reset on leave | Not ported | Cosmetic only; no gesture math depends on it |
| Splash/About guard | `about(ABOUT_END)` + `fsv_mode == FSV_SPLASH` early-outs | Not ported | This frontend has no About/splash presentation at all (Task 3.3); both checks are permanently dead code here |
| HiDPI coordinate scale | `gtk_widget_get_scale_factor()` | `SDL_GetWindowPixelDensity()` | Applied identically to click *and* drag-delta math, matching viewport.c's own scaled `prev_x`/`prev_y` |
| Pointer capture across window bounds | GTK/X11's implicit per-widget grab on button-press | Explicit `SDL_CaptureMouse(true)` on middle-press or Ctrl+left-press, released on the matching button-up | SDL3 also auto-captures while any button is held (`SDL_HINT_MOUSE_AUTO_CAPTURE`, default on) — the explicit calls are kept anyway, both because the brief asks for them and so the intent doesn't rest silently on a hint default |
| ImGui ownership | N/A | `ImGui::GetIO().WantCaptureMouse` gates every case; an in-progress capture keeps running even if the cursor is now over an ImGui window | New concern this port introduces; GTK had no overlapping widget to arbitrate against |
| Node-id → GNode* lookup | Private `node_table` static in `viewport.c` | `viewport_node_for_id()`, new — added to `src/viewport.h`/`src/sdl/stubs.c` | The table itself already lived in `stubs.c` (Task 3.3); this task added the read-back accessor `node_at_cursor()` needs. GTK frontend never calls it |

### What's live vs. plumbed-but-inert

**Fully functional today:** middle-drag dolly, Ctrl+left-drag revolve,
scroll-wheel dolly, ImGui-window event ownership (`WantCaptureMouse`),
mouse capture across window bounds, hover highlighting/status-bar text
(once `gpu_pick()` can name a node).

**Plumbed but inert until Task 4.2** (`gpu_pick()` is still a stub
returning 0 unconditionally): left-click selection and fly-to, right-click
context menu, hover highlighting/status-bar text (all depend on
`node_at_cursor()` resolving to a real `GNode*`, which cannot happen
while every pick reads back id 0 = "nothing there"). Each path is
exercised on every run without crashing; left-click logs its outcome
(`input: left-click pick at (x,y) -> no node (gpu_pick stub; Task
4.2)`) once per click. Right-click's log/`filelist_show_entry()` path
is code-reviewed but could not be *live*-exercised in this task's
testing, for the same reason: `g_indicated_node` can never be non-NULL
while the stub returns 0, so the `btn3 && g_indicated_node != NULL`
branch never fires yet.

### Verification

Two build trees, `-Dfrontend=sdl` and the GTK default, both from
scratch:

- **SDL arm:** `ninja -C builddir-sdl` — clean, zero warnings from
  `main.cpp`/`input.cpp`. `meson test -C builddir-sdl scanfs` → `1/1
  fsv:scanfs OK`.
- **GTK arm:** `ninja -C builddir-gtk` — 8 targets (no `src/fsv` on
  macOS, same pre-existing gate as every prior task), zero touch of
  `input.cpp`. `meson test -C builddir-gtk scanfs` → `1/1 OK`.
  `git diff --stat src/viewport.c` is empty.

**Headed run, real directory (`src`), synthetic `SDL_PushEvent`
injection** (screencapture is still blocked in this sandbox — same TCC
restriction as every prior task; used the same offscreen-texture
`gpu_screenshot_begin/end` capture the binary already exposes via
`--screenshot`, temporarily driven by a scripted event sequence and
reverted before commit, plus direct `camera->theta/phi/distance`
logging for an unambiguous numeric readout alongside the images):

| Step | camera state | Screenshot |
|---|---|---|
| Intro pan settled | `distance=1212.7` | `01-before-dolly` |
| Inject middle-down, 3× motion (Δy +40 each), middle-up | `distance=950.1` (**decreased** — dragging down dollies toward the target, matching `camera_dolly(-dy)` with `dy>0`) | `02-after-dolly` |
| Inject left-click at the same point | log: `input: left-click pick at (640,400) -> no node (gpu_pick stub; Task 4.2)`, no crash | — |
| Inject wheel (`y=+3`) | `distance=727.4` (**decreased further**, per this task's own scroll-up-zooms-in convention) | `04-after-wheel` |
| Inject left-click again | same stub log, no crash | — |
| Show a real ImGui window over the cursor, settle hover, inject middle-down+drag+up *over that window* | `distance=727.4` (**unchanged**) | `05-after-imgui-drag`, byte-identical file size to `04` |

The dolly and wheel screenshots show the pedestal-landscape scene
visibly closer between each step, matching the logged distance
decreases. The ImGui-capture screenshot is pixel-identical to the one
before it, confirming `WantCaptureMouse` correctly blocked navigation.

**Idle CPU** (production binary, no test scaffolding, intro pan
settled): **0.6–0.7%** (`ps`), matching Task 3.2/3.3's baseline —
`input_handle_event()` adds no per-frame cost, only per-event.

**Verification gap, disclosed rather than glossed over:** this
sandbox's SDL/macOS backend resyncs real keyboard-modifier state from
the OS on every event pump, which overwrites a programmatic
`SDL_SetModState()` (and an injected `SDL_EVENT_KEY_DOWN`/`_UP` pair)
before the paired mouse-motion events are actually drained on a later
poll cycle — confirmed by a debug trace showing the override reading
back correctly immediately after the call, then reading back as
cleared one poll cycle later. With no real Ctrl key physically held,
Ctrl+left-drag revolve could not be live-verified end-to-end through
synthetic event injection the way the other gestures were (an
`osascript`/System Events attempt to send a genuine OS-level modifier
key was also tried and blocked by the sandbox's Accessibility/TCC
restrictions, the same class of restriction that blocks
`screencapture`). Indirect evidence it is wired correctly: in the same
test run, the injected drag was correctly dispatched to the
*"pointless dragging"* branch instead (since `ctrl_key` read `false`,
exactly as SDL reported it) and correctly left the camera unchanged —
proving the branch dispatch itself is sound, just not proving the
specific Ctrl-held branch's `camera_revolve()` call fired. That call is
otherwise identical in shape to the already-proven dolly branch (same
sensitivity-scaled delta from `g_prev_x`/`g_prev_y`, same
call-and-clear-node pattern), differing only in which already-verified
`camera.c` entry point it calls.

### Deviations from the task brief

1. **The brief's gesture list doesn't match viewport.c.** See "What
   viewport.c actually does" above — there is no middle-drag "flight",
   no Shift modifier, and no double-click "activate" in the 3D
   viewport. Ported what the file actually does instead of what the
   brief assumed it does; the scroll wheel (which the brief also asks
   for, and which the checked-in verification bar explicitly wants
   screenshotted) is implemented as a clearly-labeled addition, not a
   port.
2. **`viewport_node_for_id()`, new** in `src/viewport.h`/
   `src/sdl/stubs.c` — the id→`GNode*` read-back `node_at_cursor()`
   needs. The node table itself already lived in `stubs.c` (Task 3.3);
   this is the accessor Task 4.2's real `gpu_pick()` will resolve
   through, same as `node_at_cursor()` already does today against the
   stub's 0.
3. **Cursor icon swaps dropped** (GTK's `GDK_DOUBLE_ARROW`/`GDK_FLEUR`/
   reset-on-leave). Purely cosmetic, no gesture math depends on it, and
   SDL system-cursor plumbing wasn't otherwise needed by this task.
4. **Release-event button/modifier reads use the event's own fields**
   (`ev->button.button == SDL_BUTTON_LEFT`, `SDL_GetModState()`) rather
   than reproducing GDK's X11-specific "state bitmask still reports the
   releasing button as down" convention, which SDL's per-button release
   event has no equivalent of. The natural reading of the same intent.
5. **GTK's `!gtk_events_pending()` motion-event throttle is not
   ported.** It exists to avoid working from a stale coordinate under
   an event backlog; `SDL_PollEvent()` already delivers one event at a
   time, and dropping it only ever costs a few redundant
   `node_at_cursor()` calls under an unusually fast drag, never
   correctness.

## Task 4.2 verification (gpu_pick — color-ID picking readback)

`gpu_pick(x, y)` (`src/sdl/gpu.cpp`) replaces the stub Task 3.2 left in
place, as a direct port of `ogl_select_modern()` (`src/ogl.c:456`).
`src/sdl/input.cpp`'s `node_at_cursor()` and the two debug `SDL_Log()`
placeholders it fed (button-down left-click, right-click) are removed —
they were standing in for exactly this. Nothing on the GTK arm changed:
`ogl-gpu-compat.c` already documents `gpu_pick()` as SDL-only, and
`viewport.c` still calls `ogl_select_modern()` directly (confirmed by
`grep -n "gpu_pick\|ogl_select_modern" src/viewport.c src/ogl.c
src/ogl-gpu-compat.c`).

### How it renders the id pass

No new pipeline or shader: Task 3.2 already built `pipeline_for()` with
a `target` dimension (0 = swapchain format, 1 = `R8G8B8A8_UNORM`), and
`node_set_color()` (`src/geometry.c`, survived Task 3.3 unpruned) already
branches on `gpu_render_mode()` to paint flat id colors with lighting off
instead of lit real ones. `gpu_pick()` only had to drive those two
existing seams:

1. Create a private `R8G8B8A8_UNORM` texture, swapchain-sized, and point
   `g_capture_texture` at it — reusing `gpu_screenshot_begin()`'s own
   trick, since `gpu_scene_begin()` already knows to redirect
   `g_color_target`/`g_target_index` at whatever `g_capture_texture`
   holds (see `gpu_scene_begin()`'s first two lines).
2. Set `g_render_mode = FSV_RENDER_SELECT`.
3. `gpu_scene_begin(); geometry_draw(FALSE); gpu_scene_end();` — no text,
   no cursor, exactly what `ogl_select_modern()`'s own
   `geometry_draw(FALSE)` call skipped.
4. Restore `g_render_mode`/`g_capture_texture`, then
   `SDL_DownloadFromGPUTexture()` the 1×1 region at `(x, y)` through a
   4-byte transfer buffer, fenced with
   `SDL_SubmitGPUCommandBufferAndAcquireFence()` +
   `SDL_WaitForGPUFences()`.

One behavioral fix was needed to make this correct: `gpu_scene_end()`
hardcoded the visible frame's dark-slate clear color
(`{0.08, 0.10, 0.12, 1.0}`, Task 2.2) for every pass, id or not. Its
non-zero bytes would have decoded as a bogus id for any pixel nothing
draws over — e.g. every click on empty sky. `gpu_scene_end()` now
clears to `(0,0,0,0)` when `g_render_mode == FSV_RENDER_SELECT`,
matching `ogl_select_modern()`'s own `glClearColor(0,0,0,0)` before its
pick draw.

### Byte order

`node_set_color()`'s encode (`r = id & 0xFF`, `g = (id>>8) & 0xFF`,
`b = (id>>16) & 0xFF`) and `ogl_select_modern()`'s decode
(`color[0] + (color[1]<<8) + (color[2]<<16)`) already agreed with each
other pre-port; `gpu_pick()` decodes the same way
(`pixel[0] | pixel[1]<<8 | pixel[2]<<16`) and the round-trip is confirmed
by the click tests below resolving to the exact node drawn at that
screen position.

### Y-flip finding

**No flip is needed**, unlike `ogl_select_modern()`'s
`yy = viewport[3] - y`. That subtraction exists purely to convert into
`glReadPixels()`'s bottom-left-origin convention. SDL_GPU texture
regions are top-left origin — `SDL_gpu.h`'s `SDL_GPUTextureRegion::y`
is documented as "the *top* offset of the region" — the same convention
the swapchain and `input.cpp`'s `(x, y)` already use (this matches the
Y-flip finding Task 3.1/3.2 already made for the swapchain itself).
Verified empirically, not just read off the header: clicking a node near
the top of the window (`xmaps`, a folder pedestal) and a node mid-window
(`camera.c`) both resolved to the correct node; an inverted flip would
have swapped which one hit which.

### Own-command-buffer design

`gpu_pick()` acquires its own `SDL_GPUCommandBuffer` rather than reusing
whatever the main loop is doing, and temporarily repoints the module's
shared `g_cmd`/`g_color_target`/`g_target_index`/`g_render_mode`/
`g_capture_texture` state at its own private target for the duration of
one `gpu_scene_begin()`/`geometry_draw()`/`gpu_scene_end()` call, then
restores it. This is safe — not just convenient — because
`input_handle_event()` (the only caller, via `node_at_cursor()`) always
runs inside `main.cpp`'s `SDL_PollEvent()` loop, strictly before that
iteration's own `gpu_frame_begin()`/`submit_frame()`: there is never an
in-flight scene command buffer at the point a pick can happen. `gpu_pick()`
checks `g_cmd != nullptr || g_recording` on entry and turns a violation of
that invariant into a logged no-op pick rather than corrupting whichever
frame runs second.

### What's live now

`node_at_cursor()`'s hover path, left-click select+highlight, and
button-up `camera_look_at()` fly-to are no longer plumbed-but-inert
(Task 4.1's own description) — they resolve real nodes end to end.
Right-click's `filelist_show_entry()` call is exercised for real too
(still a no-op body until Task 5.2); its ImGui context-menu is still
Task 5.1.

### Manual verification

Built both arms from scratch (`meson setup builddir && ninja -C
builddir` for the GTK/headless arm — the `fsv` GTK executable itself is
gated off on Darwin, pre-existing and unrelated to this task, see
`src/meson.build`; `meson setup builddir-sdl -Dfrontend=sdl && ninja -C
builddir-sdl` for the Metal arm). Zero warnings from `gpu.cpp`/
`input.cpp`/`main.cpp`. `meson test -C builddir scanfs` and `meson test
-C builddir-sdl scanfs` both → `1/1 fsv:scanfs OK`.

Verified with a temporary, fully-reverted hook in `--screenshot` mode
(env-var gated, `git diff src/sdl/main.cpp` is clean of it — the
committed diff there is a one-comment update): direct `gpu_pick(x, y)`
calls cross-checked against a screenshot of `src` (this repo) rendered
in MapV, and a full `input_handle_event()` drive (synthetic
`SDL_Event`s, no real event queue needed) to prove the production
hover/click/fly-to path, not just `gpu_pick()` itself:

- `gpu_pick(480, 445)` on the `src` MapV pedestal layout → id 9 →
  `src/camera.c`, matching the visibly labeled pedestal at that screen
  position.
- `gpu_pick(676, 445)` → id 37 → `src/gui.c`, same layout, adjacent
  pedestal.
- `gpu_pick(598, 150)` (near the top of the window) → id 60 →
  `src/xmaps`, the folder pedestal drawn there — this is the pick that
  falsifies an inverted Y-flip (see above).
- `gpu_pick(100, 100)` on empty sky → id 0, no node, no crash.
- Same four checks repeated on `tests/fixture` (smaller tree): the
  visible `file1.txt` pedestal resolved correctly by id, empty sky → 0.
- Full `input_handle_event()` drive at `(480, 445)` (the `camera.c`
  pedestal): a synthetic hover motion event visibly brightens
  `camera.c`'s pedestal in a screenshot (`geometry_highlight_node()`
  through the real `node_at_cursor()` path, not a direct `gpu_pick()`
  call) — confirms indicated_node/hover highlighting is live end to
  end. A synthetic press+release pair then reports `camera_moving() ==
  1` immediately, and a further 3s of ticked animation shows the camera
  visibly closer to `camera.c` in a screenshot pair — `camera_look_at()`
  fires from the real button-up handler.
- Same drive at `(100, 100)` (empty sky): `camera_moving() == 0` before
  and after, no crash — clicking nothing does nothing, as it should.
- Screenshots taken immediately after every `gpu_pick()`/click call
  (the normal `--screenshot` capture that already follows in that code
  path) are visually identical to a pick-free run of the same
  directory — the id-color pass never reaches the visible frame,
  confirming the private-target design doesn't leak into what the user
  sees.
- Pick latency, timed around the `gpu_pick()` call only (three repeated
  clicks on the same node, same process, timer code not kept in the
  committed diff): 1.85 ms, 9.29 ms, 6.79 ms — the higher one is the
  first call, which pays pipeline_for()'s one-time lazy-build cost for
  the `target == 1` combination; all three are far under the 50 ms
  click-frequency budget.

### Fix round (post-review)

Code review on the original submission approved the byte order, Y-flip,
command-buffer isolation, and clear-color fix, but found one Important
issue and one Minor one:

1. **TreeV branch/loop connectors leaked a bogus id into the pick
   pass.** `treev_gldraw_loop()`/`_inbranch()`/`_outbranch()`
   (`src/geometry.c`) draw the branch geometry connecting platforms via
   `draw_lit(FSV_TRIANGLE_STRIP, vert, vert_cnt, &branch_color, NULL)` —
   the `color != NULL` path, used for fixed-color batches that have no
   backing `GNode`. That path called `gpu_set_color()` directly with no
   render-mode check at all, so during the select pass it painted
   `branch_color` (`{0.5, 0.0, 0.0}`) — a color that decodes to a
   non-zero node id (confirmed empirically at 126, close to the
   predicted 128 for `0.5 * 255`) rather than "nothing here". Clicking
   a branch connector in TreeV therefore silently selected whatever
   node happened to occupy that id's slot in the node table — wrong,
   and non-obviously wrong, since it never crashed.

   Fixed in `draw_lit()` itself (the one place this branch exists),
   using the same `gpu_render_mode()` check `node_set_color()` already
   uses: in the select pass, draw fixed-color geometry as id 0 (black,
   unlit) instead of skipping it. Skipping was considered and rejected
   — it would drop the connector from the pick pass's depth buffer,
   letting a click *through* a branch to whatever node sits behind it,
   which would make pick-pass occlusion diverge from what is actually
   on screen. Id 0 keeps the occlusion (the connector still blocks the
   depth test) while correctly reporting "not a node" for a batch that
   never was one. `draw_unlit()` needed no equivalent change: every
   current caller already passes `&color_black`, so it was already
   painting id 0 in the select pass by construction — documented in
   its own comment now rather than left as an unstated fact.

   Verified the fix actually closes the reported bug, not just an
   incidental zero: reverted the `draw_lit()` change, rebuilt, and
   re-clicked the same branch-connector pixel — got id 126 (bogus)
   again — then restored the fix and confirmed id 0.

2. **Re-verified every other `draw_lit`/`draw_unlit` call site with a
   fixed (non-node) color** for the same class of bug (`grep -n
   "draw_lit(\|draw_unlit(" src/geometry.c`): MapV's folder outline
   (`mapv_gldraw_folder`) and TreeV's leaf "X" mark both pass
   `&color_black` to `draw_unlit()` — benign, per the point above.
   DiscV's folder draw (`discv_gldraw_folder`) is an empty stub ("To be
   written...") — draws nothing, so nothing to guard. The node cursor
   (`mapv_draw_cursor`/`treev_draw_cursor`, drawn via raw `gpu_draw()`
   calls with colors from `cursor_hidden_part()`/`cursor_visible_part()`
   — gray and white, not black) is gated behind `if (high_detail)` in
   all three of `discv_draw()`/`mapv_draw()`/`treev_draw()`, and
   `gpu_pick()` always calls `geometry_draw(FALSE)` — so the cursor
   never draws during a pick regardless of its color, confirmed by
   reading all three `*_draw()` functions, not just assumed. No fix
   needed there today; if a future task lets the cursor draw during a
   select pass, it would need the same treatment as the branch
   connectors, not the treatment `draw_unlit()` already has for free.

3. **Minor: a failed fence acquire in `gpu_pick()` fell through to a
   read anyway.** If `SDL_SubmitGPUCommandBufferAndAcquireFence()`
   returned `nullptr`, the old code skipped the wait (nothing to wait
   on) and went straight to `SDL_MapGPUTransferBuffer()`, which could
   return stale or undefined bytes from a copy that was never known to
   have landed — decoding into a plausible-looking but bogus id
   instead of a signaled failure. Now logs and returns 0, releasing the
   transfer buffer and pick texture first, matching every other failure
   path in the function.

Re-verified after both fixes: both arms rebuild clean
(`ninja -C builddir`, `ninja -C builddir-sdl`), zero warnings;
`meson test scanfs` → `1/1 OK` on both. Headed TreeV test on `src`
(1280×800, `--treev`): clicking the red branch stem at `(645, 650)` →
id 0, no selection; clicking leaf tiles at `(392, 445)` and `(566,
178)` → `src/TODO` and `src/gpu.h` respectively, both correct; empty
sky at `(100, 100)` → id 0. Re-ran the MapV `camera.c` pick at `(480,
445)` from the original verification → still id 9 → `src/camera.c`,
confirming no regression. The visible TreeV render (a normal,
non-`--screenshot`-mode-agnostic frame) is pixel-identical before and
after the fix — the branch connector is still visibly red — confirming
the id-0 substitution is select-pass-only and never leaks into what the
user sees.

## Task 5.1 verification (ui_main.cpp — ImGui menu bar and mode switching)

`src/sdl/ui_main.h`/`ui_main.cpp` replace the GTK menu shell -- `src/gui.c`'s
menu-widget helpers, wired up in `src/window.c`'s `window_init()`, with
the actions themselves in `src/callbacks.c` -- with an ImGui main menu
bar offering the same actions, adapted where GTK-specific (a native
folder dialog instead of `GtkFileChooser`, an ImGui popup instead of a
`GtkMenu`). `src/sdl/app.h` (implemented in `main.cpp`) is the seam
`ui_main.cpp` calls through for mode switching and root-directory
changes, so this task does not duplicate `main.cpp`'s
`load_filesystem()`/`enter_mode()` (Task 2.2/3.3). No GTK file changed
(`git diff --stat -- src/gui.c src/window.c src/callbacks.c src/fsv.c
src/dirtree.c src/colexp.c src/color.c src/viewport.c` is empty).

### Menu -> GTK original cross-reference

| Menu item | GTK original | Notes |
|---|---|---|
| File -> Change Root... | `on_file_change_root_activate()` -> `dialog_change_root()` | `SDL_ShowOpenFolderDialog()` instead of `gtk_file_chooser_dialog_new()`; defaults to the current root, like `dialog_change_root()`'s own default |
| File -> Rescan | *(none)* | Addition: GTK's menu has no separate "just rescan" action, only Change Root. Re-scans `app_root_dir()` in place |
| File -> Quit | `on_file_exit_activate()` -> `exit(EXIT_SUCCESS)` | Pushes a real `SDL_EVENT_QUIT` instead of calling `exit()` directly, so `main.cpp`'s loop still runs its normal shutdown (`gpu_shutdown()`, `SDL_Quit()`) and its mid-scan-quit handling |
| Vis -> DiscV/MapV/TreeV | `on_vis_*_activate()` -> `fsv_set_mode()` | `app_switch_mode()` is `fsv_set_mode()`'s non-`FSV_NONE` branch, ported into `main.cpp` next to `enter_mode()` (the `FSV_NONE` branch) so the two share `initial_camera_pan()` instead of duplicating it |
| Colors -> By node type/timestamp/wildcards | `on_color_by_*_activate()` -> `color_set_mode()` | Direct call, unchanged core API |
| Colors -> Setup... | `on_color_setup_activate()` -> `dialog_color_setup()` | Disabled placeholder, "(Task 5.3)" -- menu-level switching only, per the brief |
| Help -> Controls | *(none)* | Addition: a window listing input.cpp's real, verified gesture table (docs/PORTING.md's own Task 4.1 section), not the brief's original guess at the gestures |
| Help -> About fsv... | `on_help_about_fsv_activate()` -> `about(ABOUT_BEGIN)` | A plain ImGui window (version/lineage text) instead of the 3D splash presentation, which this frontend has never had (Task 3.3) |
| Right-click on a node | `context_menu()` (GTK popup) | ImGui popup: node name, Look At (`camera_look_at()`), Properties (disabled, "Task 5.3"), Collapse/Expand (`colexp()`, directories only) -- see the seam below |

### Mode switching: reusing, not duplicating, `enter_mode()`

`fsv_set_mode()` (`src/fsv.c`) has two branches: `FSV_NONE` (the
filesystem's first appearance -- `first_init = TRUE`, the slow 4-second
fly-in) and everything else (a same-filesystem mode switch -- short pan
from wherever the camera already is, TreeV getting an L-shaped one via
`camera_treev_lpan_look_at()`). `main.cpp`'s `enter_mode()` (Task 2.2/3.3)
was only ever the first branch. Rather than write a second, parallel
`initial_camera_pan()` for `app_switch_mode()`, `initial_camera_pan()`
itself was extended to switch on its `mesg` argument exactly as
`fsv.c`'s original does (`"new_fs"` -> root fly-in; anything else ->
TreeV lpan or the short `MORPH_INV_QUADRATIC` pan), and `app_switch_mode()`
schedules it with `""` the same way `fsv_set_mode()`'s non-`FSV_NONE`
branch does. `app_switch_mode()` itself is a direct port of that
branch's `geometry_init()` + `camera_init(mode, FALSE)` +
`schedule_event()` sequence, guarded the same way
`on_vis_*_activate()` guards it (`if (globals.fsv_mode != mode)`), plus
a `g_scanning` guard `fsv_set_mode()` never needed (GTK's menu is
disabled during a scan via `window_set_access(FALSE)`; this frontend has
no menu bar drawn at all during a scan -- see below).

### Deferred Rescan/Change Root: avoiding an ImGui re-entrancy bug

`app_show_change_root_dialog()`/`app_request_rescan()` do not call
`scanfs()`. They only set a pending-request flag, consumed by a new
`app_apply_pending_root_change()` that `main.cpp`'s loop calls right
after `submit_frame()` -- i.e. once the frame that queued the request is
fully closed out. This is necessary, not just tidy: `scanfs()` drives its
own progress overlay through `gui_update()`, which (Task 3.3's
finding 2) calls `imgui_new_frame()`/`ImGui::Render()`/`submit_frame()`
itself, gated on `g_scanning`. `ui_main_draw()` runs *inside* the main
loop's own `imgui_new_frame()`/`ImGui::Render()` pair -- calling
`scanfs()` (and therefore `gui_update()`) synchronously from a menu-item
click handler there would call `ImGui::NewFrame()` a second time before
the first pair had closed, which ImGui does not support. Deferring the
actual scan to after `submit_frame()` sidesteps this entirely: by the
time `scanfs()` runs, there is no open frame to re-enter, exactly the
condition the main loop's own top-level call already relies on. One
side effect worth naming: while a deferred scan runs, `ui_main_draw()`
is not called at all (it's not the current loop iteration) — the menu
bar is not merely greyed out, it does not exist on screen for that
window, which is a stronger version of GTK's `window_set_access(FALSE)`
menu-disable and needs no separate implementation.

### A real bug the first Rescan attempt found: `scanfs()`'s permanent `chdir()`

`src/scanfs.c:320` `chdir()`s into the scanned directory and never
`chdir()`s back. The GTK frontend never notices, because
`dialog_change_root()` only ever hands `fsv_load()` an *absolute* path
(`gtk_file_chooser_get_filename()`'s contract) and never re-scans the
same string twice in a row. `load_filesystem()` here first stored the
caller's `dir` argument verbatim as the tracked root; Rescan then handed
that same (possibly relative, e.g. `"src"`) string back to `scanfs()`,
which `chdir()`'d *again*, this time relative to the directory the first
scan had already left the process in -- looking for `.../src/src` and
fatally `g_error()`-ing (`g_error()` is fatal by default in glib; the
whole process aborted mid-test). Fixed by storing `xgetcwd()`'s result
(the resolved absolute path `scanfs()` itself computes right after its
own `chdir()`), not the caller's string, as `g_root_dir`. Reproduced
before the fix (killed the process) and confirmed absent after,
end-to-end, via the harness described below. `SDL_ShowOpenFolderDialog()`
was never affected (its result is always absolute already) -- this was
strictly a Rescan-on-a-relative-root bug.

### Context-menu seam

`src/sdl/input.h`'s new `ContextMenuRequest` (`{ pending, node, x, y }`,
`node` kept as `void *`) is written only by `input.cpp`'s existing
right-click branch (`SDL_EVENT_MOUSE_BUTTON_DOWN`, `btn3 &&
g_indicated_node != NULL` -- Task 4.1/4.2) and read/cleared only by
`ui_main.cpp`'s `draw_context_menu()`, once per frame. Kept one-way (the
same shape as `viewport_node_for_id()`, Task 4.1's own seam) so
`input.cpp` never has to know ImGui exists beyond the
`WantCaptureMouse` check it already made; `node` stays `void *` rather
than `GNode *` so `input.h` stays glib-free, the same reasoning behind
its SDL-only, `common.h`-free style since Task 4.1.

### Window title

`SDL_SetWindowTitle(g_window, "fsv - <root>")` on every successful
`load_filesystem()` (startup, Rescan, Change Root) -- GTK's window is
always titled a bare `"fsv"` (`src/window.c:175`); this is a deliberate
addition over the original, not a port, per the brief.

### Manual verification

Both arms build clean from scratch:

- `meson setup builddir-sdl -Dfrontend=sdl && ninja -C builddir-sdl` --
  31/34 targets rebuilt after `rm -rf`, zero warnings from any file this
  task touched (`main.cpp`, `input.cpp`, `input.h`, `ui_main.cpp`,
  `ui_main.h`, `app.h`).
- `meson setup builddir-gtk && ninja -C builddir-gtk` -- same
  pre-existing Darwin gate on the GTK executable itself as every prior
  task; `libfsvcore`/`fsv-scan`/`test_scanfs` build clean.
- `meson test scanfs` -> `1/1 OK` on both arms.

Headed verification used a temporary, `FSV_UI_TEST`-env-var-gated script
in `main.cpp` (real `SDL_PushEvent()`-injected mouse motion/clicks routed
through the actual `ImGui_ImplSDL3_ProcessEvent()` + `input_handle_event()`
path, not direct ui-state calls -- the brief's preferred option, tried
first and it worked) plus matching temporary rect-capture logging in
`ui_main.cpp` (`ImGui::GetItemRectMin()/Max()` right after each
`BeginMenu()`/`MenuItem()`, so the script could click the *actual* pixel
each menu item rendered at, discovered live in the same run rather than
hand-guessed). Both fully reverted before commit (`git status --short`
lists only the real feature files; `grep -rn "TEMPORARY\|UITEST"
src/sdl/*.cpp src/sdl/*.h` is empty).

Full scripted run against `src` (this repo, 1280x800): raise window ->
click File (rects `file=(26,10) vis=(66,10) colors=(114,10)
help=(165,10)`) -> click Rescan -> click Vis -> click DiscV -> click
Vis -> click TreeV -> click Vis -> click MapV -> click Colors -> click
By timestamp -> click Colors -> click By node type -> click Help ->
click Controls -> click Help -> click About fsv... -> right-click
`(480, 445)` (Task 4.2's known `src/camera.c` pedestal) -> click Look At
-> `app_request_change_root()` to `tests/fixture`'s absolute path (the
brief's explicit allowance: the native dialog itself can't be driven in
this sandbox, so this calls the exact code path its callback would) ->
click File -> click Quit. Every step logged internal state via
`SDL_Log()`:

| Step | Logged evidence |
|---|---|
| Rescan | `root=/…/fsv/src` before and after, `fstree` pointer changed (fresh scan, not a no-op) -- and, pre-fix, this step is what surfaced the `chdir()` bug above |
| Vis -> DiscV | `mode=0` (`FSV_DISCV`), `distance=1804.6` |
| Vis -> TreeV | `mode=2` (`FSV_TREEV`), `distance=23500.5` (TreeV's much larger camera scale) |
| Vis -> MapV | `mode=1` (`FSV_MAPV`), restored |
| Colors -> By timestamp / By node type | both switch with no crash; scene screenshots (below) show a visibly different palette between them |
| Right-click `(480,445)` | context menu's captured node name: `/…/fsv/src/camera.c` -- the exact node Task 4.2 verified sits at that pixel |
| Look At | `distance` 1158.9, `theta`/`phi` changed from the pre-click values -- the camera moved |
| Change Root | `root=/…/fsv/tests/fixture`, window title `"fsv - /…/fsv/tests/fixture"`, `fstree` pointer changed again |
| Quit | process exited (0), no crash, no leaked child |

**Scene-only screenshots** (via the existing `gpu_screenshot_begin/end`
path, unaffected by ImGui) independently confirm the mode/color switches
visually: DiscV renders the ringed-disc layout, TreeV the platform with
its red branch stem and "src" label, MapV the pedestal landscape,
By-timestamp recolors pedestals into a red/orange/yellow gradient
distinct from By-node-type's uniform pale yellow, and both Change-Root
screenshots show the same "tall thin tower" MapV shape Task 3.3's report
documented for `tests/fixture` specifically (a 5-file tree against a
fixed `mapv_dir_height`) -- independent confirmation the new root's
geometry, not the old one, is what got drawn.

**Idle CPU**: 0.8% (`ps`) on `src`, intro pan settled, no `FSV_UI_TEST`
-- unchanged from prior tasks' baseline.

**Verification gap, disclosed rather than glossed over**: this
sandbox's `screencapture -x` (full-screen, no window targeting) returned
a solid-black image in this session, for the *entire* screen, not just
the fsv window -- a harder version of prior tasks' "window occluded"
finding (see the decision log). This means the menu bar, the node
context-menu popup and the About/Controls windows were verified to
*exist and contain the right data* (the item-rect capture only succeeds
if the corresponding widget was actually drawn that frame; the context
menu's captured text was the real, correct node name; state changes
tracked via `SDL_Log()` are unambiguous) but their **pixels** were not
visually inspected -- only the 3D scene's pixels were (via the
`gpu_screenshot_*()` path, which does not composite the ImGui pass and
so was never affected by this gap in the first place).

### Fix round (post-review): Retina/HiDPI context-menu misposition

Code review approved menu parity, the `chdir()` fix and the dialog
thread-safety marshaling, and required one Critical and one Minor fix
before sign-off.

**Critical -- pixel-space coordinates reached an ImGui API.**
`input.cpp` filled `ContextMenuRequest.x/y` with the same
`pixel_scale()`-multiplied coordinates it uses for `gpu_pick()`, but
`ui_main.cpp` fed that value straight into
`ImGui::SetNextWindowPos()`, which wants **logical** window
coordinates -- `imgui_impl_sdl3.cpp` sizes `io.DisplaySize` from
`SDL_GetWindowSize()`, not `SDL_GetWindowSizeInPixels()`, and never
density-scales incoming mouse events. On a 2x Retina display (this
port's primary target) a right-click at logical `(400, 300)` would have
opened the popup at `(800, 600)` -- detached from the cursor or off the
window entirely. Invisible in this sandbox's 1x virtual display, which
is exactly why it needed a reviewer with the actual failure mode in
mind, not just a headed run, to catch.

Fixed by carrying **logical** coordinates in the seam instead of pixel
ones (`input.h`'s `ContextMenuRequest.win_x/win_y`, `float`, filled from
`ev->button.x/y` directly -- not the scaled `x, y` locals `input.cpp`
had already spent on the pick). No pixel-space field was added back in:
the pick itself has already happened by the time the struct is filled,
so nothing downstream needs that space again. Both `input.h` (the
struct's own doc comment) and `input.cpp`'s `pixel_scale()` (which
originates the first half of the trap) now cross-reference each other
and spell out which space each value in the file belongs to and why --
per review's own framing, "the second pixel-vs-point trap in this
codebase," so the comment is written to save the third person from
finding it the hard way.

**Sweep for other crossings**: grepped every `ImGui::SetNextWindowPos/
SetNextWindowSize/SetCursorScreenPos` call and every
`pixel_scale()`/`SDL_GetWindowSizeInPixels()`/`SDL_GetWindowSize()`
call across `src/sdl/*.cpp`. The scan-progress overlay's
`SetNextWindowPos()` (`main.cpp`) positions off `ImGui::GetMainViewport()
->WorkPos`, ImGui's own logical-space viewport rect -- never touches
`pixel_scale()`, so it was never at risk. Every `SDL_GetWindowSizeInPixels()`
call feeds either GPU texture sizing (`gpu.cpp`) or `camera.c`'s aspect
ratio via `sdl_viewport_size()` (`main.cpp`) -- both legitimately pixel-
space consumers, no ImGui involved. The context-menu seam was the only
crossing.

**Minor -- undocumented divergence from `fsv_set_mode()`.**
`app_switch_mode()` never had a call matching `fsv_set_mode()`'s
`about(ABOUT_END)` ("ensure the About presentation is not up"). Not a
functional gap -- this frontend's `about()` (`stubs.c`) is an
unconditional `FALSE` no-op, so the call would be permanently dead code
-- but it went unremarked. A one-line comment now names the omission at
the call site.

**Covering verification**: both arms rebuilt clean from scratch
(`ninja -C builddir-sdl`, `ninja -C builddir-gtk`), zero warnings;
`meson test scanfs` -> `1/1 OK` on both.

Re-ran the synthetic right-click test, extended per review's own ask --
proving the fix in a density-independent way, not just re-confirming
1x behavior. A temporary, reverted `FSV_TEST_DENSITY` env-var override
in `pixel_scale()` simulated a Retina-class density this sandbox's real
display doesn't have, letting the fix be *disproven if wrong* rather
than merely re-observed at a density where the bug is numerically
invisible:

| Run | Measured/forced density | `input.cpp` fill (logical / pixel) | `ui_main.cpp` consume (`win_x/win_y`) | Actual popup position |
|---|---|---|---|---|
| Real sandbox display | **1.0** (`SDL_GetWindowPixelDensity()`, unforced) | `(480.0,445.0)` / `(480.0,445.0)` -- identical at 1x | `(480.0,445.0)` | `(480.0,445.0)` |
| `FSV_TEST_DENSITY=2.0` | **2.0** (forced) | `(240.0,222.5)` / `(480.0,445.0)` -- diverge exactly 2x | `(240.0,222.5)` | `(240.0,222.0)` (ImGui's own float->pixel-grid snap) |

At forced 2x, the pick still resolved to the correct node -- the
injected click's logical coordinate was `480/density, 445/density` so
its *pixel* coordinate lands on the exact spot Task 4.2 verified is
`src/camera.c`, keeping the test meaningful rather than just clicking
into empty space at a different density. The popup opened at the
**logical** pair in both rows, never the pixel one -- at 1x the two
happen to coincide (which is precisely why this bug was invisible in
this sandbox before review caught it by reading the code, not by
looking at a screenshot), and at 2x they provably diverge by exactly
the density factor while the popup still tracks the logical value.
This is the density-independent proof review asked for: the assertion
that matters is "`ui_main.cpp` received the *logical* pair" — true at
both densities — not "the two numbers happened to match," which is
only true at 1x and would have been true even with the bug still
present in a 1x sandbox.

All temporary harness code (the `FSV_TEST_DENSITY` override, the
right-click injection script, and the `FSV_UI_TEST`-gated logging in
`input.cpp`/`ui_main.cpp`) was fully reverted before commit --
`grep -rn "TEMPORARY\|UITEST\|FSV_UI_TEST\|FSV_TEST_DENSITY"
src/sdl/*.cpp src/sdl/*.h` is empty.

## Task 5.2 verification (ui_panels.cpp — directory tree + file list)

`src/sdl/ui_panels.h`/`ui_panels.cpp` (new) replace `src/dirtree.c`
(a `GtkTreeView`) and `src/filelist.c` (another `GtkTreeView`, used as a
flat list) — together, the left-hand pane `src/window.c` builds via
`gui_hpaned_add()`/`gui_vpaned_add()` — with a single ImGui window: the
directory tree on top, the selected directory's contents below. Neither
GTK file is touched. This is also what turns most of `src/sdl/stubs.c`'s
`dirtree_*`/`filelist_*` no-ops into real implementations — those
symbols now live in `ui_panels.cpp`, not `stubs.c`.

### Why no persistent widget tree

`dirtree.c` owns a `GtkTreeStore`: one row per directory, built
incrementally as `scanfs.c` calls `dirtree_entry_new()`, mutated by
`dirtree_entry_expand()`/`_collapse_recursive()` as the user (or the 3D
context menu) opens/closes directories. `ui_panels.cpp` keeps no such
model: `draw_dir_node()` walks the *live* `GNode` tree directly, every
frame, descending only into directories already known to be open. That
is what makes "iterate only visible nodes" (the brief's perf
requirement) fall out for free from ImGui's own `TreeNodeEx()`/lazy-
children idiom — there is only ever one source of truth, the `GNode`
tree itself, so there is nothing to keep in sync.

The one piece of state this still needs — "is this directory's row
open" — reuses `DirNodeDesc::tnode` (`src/common.h`), the exact field
`dirtree.c` uses to remember its `GtkTreePath` for the same node.
`scanfs.c` never touches that field itself except to `NULL` it once
before the first `dirtree_entry_new()` call (`scanfs.c:333`, "needed in
dirtree_entry_new()"), so repurposing it as a plain 0/NULL-or-1/non-NULL
boolean (`tree_row_expanded()`/`set_tree_row_expanded()`) needed no core
change.

### GTK → ImGui cross-reference

| GTK original | ImGui replacement | Notes |
|---|---|---|
| `dirtree.c`'s `dirtree_select_cb()` | `draw_dir_node()`'s click branch | Ported the *asymmetric* branching verbatim: clicking an already-open row flies the camera there; clicking a closed row previews its contents in the file list instead, without moving the camera. Not a simplification of the brief's own sketch — the real behavior. |
| `dirtree.c`'s `dirtree_expand_cb()`/`dirtree_collapse_cb()` | `draw_dir_node()`'s `IsItemToggledOpen()` branch | Calls `colexp()` exactly as the GTK signal handlers do |
| `dirtree_entry_new/_show/_expand*/_collapse_recursive/_expanded` | Real implementations in `ui_panels.cpp` | Were `src/sdl/stubs.c` no-ops (one, `dirtree_entry_expanded()`, returned `dnode == root_dnode`) |
| `filelist.c`'s `filelist_populate()` | `populate_file_list()` (static) | Not exposed as `filelist_populate()`: grep confirms nothing outside `dirtree.c`/`filelist.c` ever called that symbol, so there is no core-facing contract to preserve under that name |
| `filelist.c`'s `filelist_select_cb()` | `draw_file_list_section()`'s `Selectable()` | Every click — file or directory — flies the camera there, unlike the tree above |
| `filelist_show_entry()` | Real implementation | The one core → UI notification besides the `dirtree_*` family that matters: fired from `camera.c`'s `post_pan_end()` after *every* completed pan, and from `input.cpp`'s right-click branch (Tasks 4.1/4.2, both already wired to call it before this task existed) |
| `filelist_reset_access()` | No-op | Re-reads `dirtree_entry_expanded(g_shown_dir)` live at draw time instead — an ImGui window has no persistent "insensitive" state to push a value into ahead of time |
| `window.c`'s `hpaned_w`/`vpaned_w` | One ImGui window | Tree on top (~1/3 height, matching `vpaned_w`'s `window_height/3` initial split), file list below (~2/3); panel width matches `hpaned_w`'s `window_width/5`. Brief's "simplest: one panel" option |
| *(none)* | View → "Directory Tree && Files" menu item | Addition: GTK's left pane has no show/hide toggle at all, only a paned divider the user can drag but never fully hide |

### Deliberate deviations

1. **Tree row order is structural (dir-first, size-descending), not
   alphabetical.** `dirtree.c` inserts rows in `scanfs.c`'s raw
   scan-time order (alphabetical, from `scandir()`+`alphasort()`) —
   but `draw_dir_node()` walks the *current* `GNode` children list,
   which `scanfs.c`'s `setup_fstree_recursive()` permanently re-sorts
   (dir-first, then by size) for the 3D geometry's own layout purposes,
   *after* the scan finishes. Re-sorting a directory's children to
   match GTK's alphabetical row order every frame purely for the tree
   panel would cost real time on directories with many entries and
   would first require mutating `->children` — off the table, since
   that ordering is geometrically significant (MapV/TreeV/DiscV layout
   depends on it). The **file list**, by contrast, *is* sorted
   alphabetically (`populate_file_list()`'s `compare_name`), matching
   `filelist.c`'s own `compare_node` exactly — safe because it only
   re-sorts a cached copy when the shown directory changes, never
   per frame.
2. **`dirtree_entry_expand()`/`_expand_recursive()` also open every
   ancestor**, which GTK's versions never needed to. GTK's
   `GtkTreeStore` rows all exist regardless of expansion state
   (collapsing a row just hides its children); `draw_dir_node()`'s walk
   *only* descends into rows it already knows are open, so an ancestor
   left closed would make the target node undrawable no matter what
   its own flag says. `expand_ancestors()` is the fix, called from
   both.
3. **View menu panel toggle** (see table above) — GTK has none.
4. **File list has a third column, Size**, beyond `filelist.c`'s two
   (an icon pixmap column and Name). Not requested by the GTK original
   at all — an addition, kept because the brief's own scope list asks
   for "name, size, type icon-as-text prefix" explicitly, and because
   the icon pixmap column had to be replaced with *something* once the
   Size column highlighted how little the two-column original actually
   showed. Uses `abbrev_size()` (`src/common.c`, already used by
   `dialog.c`'s Properties dialog), and — for directories — the
   subtree size (`DIR_NODE_DESC(child)->subtree.size`), not the raw
   node size GTK's own Properties dialog shows for a plain file, since
   a directory's own on-disk size is rarely the number a user browsing
   the list actually wants.

### A real bug the panel-click verification found: unbalanced `TreePop()`

`draw_dir_node()`'s first cut called `ImGui::TreePop()` whenever
`has_subdirs` was true, regardless of `node_open`. `ImGui::TreeNodeEx()`
only pushes an ID scope when `node_open` is true
(`imgui_widgets.cpp`: `if (is_open && !NoTreePushOnOpen)
TreePushOverrideID(id);`), so collapsing a directory *with* children via
a real click unbalanced the ID stack and asserted on the very next
`TreePop()` (`window->IDStack.Size > 1`) — reproduced live, not a
hypothetical, the first time the verification harness below actually
collapsed `root_dnode` via its arrow. Fixed by gating both the recursion
and the `TreePop()` on `has_subdirs && node_open` together.

### Manual verification

Both arms build clean from scratch:

- `meson setup builddir-sdl -Dfrontend=sdl && ninja -C builddir-sdl` —
  zero warnings from any file this task touched (`ui_panels.h`,
  `ui_panels.cpp`, `main.cpp`, `ui_main.cpp`, `stubs.c`,
  `meson.build`).
- `meson setup builddir-gtk && ninja -C builddir-gtk` — untouched;
  `git diff --stat -- src/gui.c src/window.c src/callbacks.c src/fsv.c
  src/dirtree.c src/filelist.c src/colexp.c src/color.c src/viewport.c
  src/dialog.c` is empty.
- `meson test scanfs` → `1/1 OK` on both.

Headed, scripted, real-SDL-event verification (a temporary
`FSV_PANELS_TEST`-gated harness in `main.cpp`, plus matching temporary
rect/state-logging hooks in `ui_panels.cpp`/`ui_main.cpp`, all fully
reverted before commit — see below) against `src` (this repo,
1280×800):

1. **Panel shows the real tree.** Per-frame logging of `root_dnode`'s
   row (name, rect, expanded state) confirms the panel renders live
   scan data from the very first frame — no separate widget model to
   go stale.
2. **Synthetic click expanding/collapsing a dir in the panel → 3D scene
   changes (screenshot proof).** Root starts open (only two real
   directories exist anywhere under `src` — `sdl` and `xmaps`, both
   leaves with no subdirectories of their own, so root_dnode is the
   only row with an arrow to click in this particular tree). A real
   `SDL_EVENT_MOUSE_BUTTON_DOWN`/`UP` pair at the arrow's computed
   hitbox (`[cursor.x, cursor.x + FontSize + 2×FramePadding.x)`, the
   same formula `TreeNodeEx()` uses internally) collapsed it —
   `dirtree_entry_collapse_recursive('src') -> tree_row_expanded=0`
   logged — then a second click re-expanded it —
   `dirtree_entry_expand('src') -> tree_row_expanded=1` logged.
   `gpu_screenshot_begin/end()` (offscreen, unaffected by ImGui
   compositing) captured all three states; MD5 of the raw pixels:
   before-collapse and after-re-expand are byte-identical
   (`58def58942db`, confirming the round trip is exact),
   after-collapse differs (`a2e691b68d9c`) — visually, TreeV's fully
   laid-out `src` tree collapses to a single flat folder box and back.
3. **Double-click/context-menu expand in 3D → panel reflects it
   (logged assertion).** A real right-click on a directory found via a
   `gpu_pick()` grid search (`xmaps`, found fresh immediately before
   the click — see below), through the *existing* Task 5.1 context
   menu's "Expand" item, logged
   `dirtree_entry_expand('xmaps') -> tree_row_expanded=1` — proving
   `colexp()`'s core-side notification reaches this file's real
   `dirtree_entry_expand()` regardless of which caller (panel arrow,
   3D context menu, or — untested here but identical code path —
   `dialog.c`'s future Properties dialog) invoked `colexp()`.
4. **File click → camera moves (screenshot pair).** Root's file list
   auto-populates once the intro pan settles on `root_dnode` (via the
   real `filelist_show_entry()` call `camera.c`'s `post_pan_end()`
   already makes) — no prior click needed. A real click on the
   `camera.c` row moved the camera (`theta` 270.00→269.97, `distance`
   1276.66→1273.55) and highlighted its pedestal — visible in the
   screenshot pair (before/after MD5s differ) as the platform turning
   from the ordinary yellow color to white/highlighted.

**Two testing-methodology findings, not application bugs** (recorded
here because they will bite the next person who tries this kind of
harness against this vendored ImGui build):

- **Synthetic mouse-button events need a preceding, separately-timed
  motion event, and press/release must land in separate rendered
  frames.** `imgui_impl_sdl3.cpp`'s `SDL_EVENT_MOUSE_BUTTON_DOWN`/`UP`
  case only calls `io.AddMouseButtonEvent()` — `io.MousePos` is set
  only by `SDL_EVENT_MOUSE_MOTION` (or a same-frame
  `SDL_GetGlobalMouseState()` fallback when no real OS mouse is
  hovering the window, which this sandbox always hits). Worse,
  `io.WantCaptureMouse` is only recomputed by ImGui's layout pass at
  the *next* `NewFrame()`, so a motion+button pair injected in the same
  batch is evaluated against *last frame's* hover position. And
  `TreeNodeEx()`'s `OpenOnArrow` toggle uses
  `ImGuiButtonFlags_PressedOnClick` (fires on mouse-down, "rather
  standard", per its own source comment) rather than
  `PressedOnClickRelease` — bundling press+release in one batch left
  `ImGui::IsItemClicked()` reporting `true` (it is a generic,
  `ButtonBehavior`-independent check: `IsMouseClicked() &&
  IsItemHovered()`) while the widget's own internal `pressed` stayed
  `false`, so nothing ever toggled. Splitting every interaction into
  three separately-timed steps (move, press, release) fixed all of
  it — and is arguably a more faithful simulation anyway, since a real
  human's press and release are never in the same rendered frame
  either.
- **A `gpu_pick()` grid search must exclude the main menu bar's screen
  row and re-run fresh after any camera movement.** `gpu_pick()` reads
  the 3D scene directly and has no idea a menu bar exists on top of
  it, so it can return a real node id for a pixel a genuine click would
  never reach (ImGui owns that row). And picks taken once, early, go
  stale the moment something later moves the camera (the file-click
  test's whole point) — a pixel that named a directory at t=0 can name
  something else, or nothing, a few seconds later.

**Large-tree sanity** (`/opt/homebrew`, `find | wc -l` → 500,497
entries — larger than Task 3.3's own ~275k-node test target): a
temporary `FSV_FRAMETIME_LOG`-gated probe forced the *entire* scanned
tree open via `dirtree_entry_expand_recursive(root_dnode)` (this file's
own flags only — no geometry/colexp side effects) and forced continuous
rendering (bypassing the idle-wait path) to get sustained frame-time
samples rather than the handful the app's by-design idle-out would
otherwise yield. Frame time settled to a stable **~33–46ms** band
(frames 481–871 of a 60s run) with the *entire* tree expanded in the
panel — a materially harder case than "partially expanded", and it does
not degrade over time or with more of the tree walked, confirming the
per-frame cost tracks *visible* rows, not the whole 500k-entry tree
(the brief's own reasoning for why "iterate only visible nodes" was
expected to fall out of `TreeNodeEx()`'s lazy-children idiom).

**Idle CPU**: 0.4% (`ps`) on `src`, intro pan settled — at or below
every prior task's baseline (Task 5.1: 0.8%).

**Reverted before commit**: `grep -rn "PANELS_TEST\|TEMPORARY\|
FSV_FRAMETIME_LOG\|g_test_\|panels_test_tick" src/sdl/*.cpp src/sdl/*.h`
is empty. The diff against the prior commit touches exactly
`ui_panels.h`/`ui_panels.cpp` (new), `main.cpp` (the `ui_panels_draw()`
call site), `ui_main.cpp` (the View menu item and the
`dirtree_entry_expanded()` label fix), `stubs.c` (`dirtree_*`/
`filelist_*` bodies removed), and `meson.build` (new source file) —
nothing else.

### Fix round (post-review): real docking, and a clipper scroll-to bug

Code review verified the sync plumbing, the `TreePop()` fix, `tnode`'s
lifetime and the O(visible) walk, but found two Important issues and
two Minors.

**Important 1 — no actual ImGui docking.** The brief says "left dock
(ImGui docking)" and the vendored ImGui is the docking branch, but the
first cut never set `ImGuiConfigFlags_DockingEnable` and had no
`DockSpace` at all — `ui_panels_draw()`'s window was a floating window
merely *positioned* at the left edge (`SetNextWindowPos/Size`,
`ImGuiCond_FirstUseEver`), not a dockable one. Fixed:

- `main.cpp`, once, at ImGui init: `io.ConfigFlags |=
  ImGuiConfigFlags_DockingEnable`. This alone makes `ui_panels_draw()`'s
  existing `ImGui::Begin("Directory Tree", ...)` dockable — no change
  to that call itself was needed.
- New `ui_panels.cpp`/`.h`: `ui_dockspace_draw()`, called from
  `main.cpp` every frame between `ui_main_draw()` and
  `ui_panels_draw()`. Submits `ImGui::DockSpaceOverViewport(0, vp,
  ImGuiDockNodeFlags_PassthruCentralNode)`, and, on the very first frame
  only, seeds a default layout via the `DockBuilder*` API (`imgui_
  internal.h` — "very early end-user API... expect this to change/
  break", its own header's words) *only if* the resulting dock node is
  still `IsEmpty()` (i.e. nothing was already restored from a previous
  run's `imgui.ini`): split left ~25% (`window.c`'s own `hpaned_w`
  ratio, `window_width/5`), `DockBuilderDockWindow("Directory Tree",
  dock_left)`, `DockBuilderFinish()`. A `GetID()` call outside any
  window would dereference a null `CurrentWindow` and crash
  (`imgui.cpp:10052-10056`) — sidestepped entirely by letting
  `DockSpaceOverViewport()` compute and return the ID itself, the same
  approach it uses internally.
- **`.ini` persistence policy, decided explicitly, not left to
  default**: `io.IniFilename` now points at `SDL_GetPrefPath("fsv",
  "fsv")` + `"imgui.ini"` (e.g. `~/Library/Application Support/fsv/
  fsv/` on macOS) instead of ImGui's own default (`"imgui.ini"`,
  relative to the process's *current directory*). The default would
  have been actively wrong here: `scanfs.c` `chdir()`s into the scanned
  root and never `chdir()`s back, so a relative `.ini` would land
  inside whatever directory the user last scanned — not a stable
  location, and Rescan/Change Root would keep relocating it underneath
  itself. A user's own drag-to-rearrange now persists across runs at a
  fixed path instead.
- `ImGuiDockNodeFlags_PassthruCentralNode` matters for more than
  cosmetics: `imgui.cpp`'s `DockNodeUpdate()` calls
  `SetWindowHitTestHole()` on the empty central node, a *real* input
  hit-test hole, not merely `NoBackground`. That is what keeps
  `WantCaptureMouse` false over the 3D scene there, unchanged from
  before docking existed.

**Important 2 — file-list scroll-to silently broken under clipping.**
`ImGui::SetScrollHereY()` in the per-row loop only ever fires for a row
the `ImGuiListClipper` actually iterates — a row `filelist_show_entry()`
(`camera.c:950`'s `post_pan_end()`, or a right-click) targets outside
the *currently visible* range is clipped away entirely, never reaching
the loop body at all, so the scroll silently never happens. Fixed with
exactly the mechanism `imgui.h` documents for this
(`IncludeItemByIndex()`, called *before* the first `Step()`): look up
the target's index in `g_file_list` and call
`clipper.IncludeItemByIndex(idx)` before entering the `while
(clipper.Step())` loop. A `std::find()` miss (the target is not a row
of *this* list — happens once per load, when `filelist_show_entry()` is
called with the shown directory itself right after the intro pan, which
is never a row within its own listing) now clears the stale pointer
immediately instead of leaving it for every future frame to rescan for
nothing.

Verified against `/opt/homebrew/bin` (1519 entries, `ls -1 | LC_ALL=C
sort | tail -1` → `zstdmt`, the same name the port's own alphabetical
sort produces), driving `filelist_show_entry()` onto that last, far
off-screen row via a temporary harness: the row's `ImGui::
IsItemVisible()` read `false` at `rect_min_y=26145.0` (a `~26000px`
scroll-target the clipper had never even considered before) on the
exact frame the scroll-to was requested, then `true` at
`rect_min_y=777.0` (inside the ~800px-tall window) on every one of the
~80 subsequent frames observed — proving both that the row is now
processed at all (`IncludeItemByIndex()` fix) and that it settles
visibly on-screen (`SetScrollHereY()`, unchanged, now actually gets a
chance to run).

**Covering re-verification** (one combined headed run per target,
temporary harness, fully reverted): both arms rebuilt clean from
scratch, zero new warnings; `meson test scanfs` → `1/1 OK` on both.

- **Docked layout, real proof, not a screenshot** (ImGui-overlay pixels
  are still unverifiable by screen capture in this sandbox — Task 5.1's
  disclosed gap): read the generated `imgui.ini` directly after a run
  against `src`. `[Window][Directory Tree]` carries `DockId=
  0x00000001,0`; `[Docking][Data]` shows `DockSpace ID=0x08BD597D ...
  Split=X` with two child `DockNode`s — `ID=0x00000001 ... SizeRef=
  319,800` (the panel's node, ~25% of a 1280-wide window) and
  `ID=0x00000002 ... SizeRef=959,800 CentralNode=1` (the passthrough
  node). Unambiguous, human-readable ground truth that this is a real
  ImGui dock, not a floating window that merely looks similar.
- **3D scene still receives clicks in the central node**: a real
  middle-button press+drag+release injected at `(800,400)` — inside the
  central node regardless of the exact split ratio (25% of 1280 is 320)
  — dollied the camera (`distance` 1270.78→1022.58 on `src`;
  8116.80→6531.49 on `/opt/homebrew/bin`), proving
  `PassthruCentralNode`'s hit-test hole, not just its transparent
  background, actually works with the dockspace now present.
- **Panel↔3D sync regression check**: a real click on `root_dnode`'s
  tree-row arrow, now at its *docked* position (`rect=(16,54)-(303,67)`
  — the row moved down from the old floating layout's `(16,35)-
  (240,48)`, an artifact of the dock node's own frame, not a bug — found
  by re-discovering the live rect rather than assuming the old floating
  coordinates still applied), collapsed `root_dnode`
  (`toggled=1,open=0`) and a second click re-expanded it
  (`toggled=1,open=1`) — the exact same `colexp()` round trip the
  original Task 5.2 verification proved, now confirmed unaffected by
  the `DockSpaceOverViewport()` change.

**Minor 3 — process deviation, acknowledged**: Task 5.2's directory-tree
and file-list panels landed in a single commit rather than one commit
per panel as the brief's "Commit per green step" asks. Both panels
share one file (`ui_panels.cpp`) and were developed and verified
together as one coherent unit (the file list's own correctness depends
on the tree's `g_shown_dir`/expansion state); splitting them into two
commits after the fact would have meant an artificial first commit with
a non-functional half (a file list with no way to select a directory
to show). Noted here rather than repeated in the original section.

**Minor 4 — file list's Size column disclosed as a deviation**: see
"Deliberate deviations" above (item 4) — added to `docs/PORTING.md`
alongside the other three rather than only in this fix-round note,
per the review's own ask.

## Task 5.3 (ui_dialogs.cpp — Color Setup, Properties; SDL default on macOS)

Ports the last two `src/dialog.c` windows — `dialog_color_setup()` and
`dialog_node_properties()` — as ImGui windows in the new
`src/sdl/ui_dialogs.cpp`/`.h`, wires them into the Task 5.1 menu bar and
context menu (removing both `(Task 5.3)`-disabled stubs), and flips
`meson_options.txt`'s `frontend` default from `gtk` to `sdl`. This
completes UI parity (Milestone 5) — every menu item and context-menu
entry Tasks 5.1/5.2/5.3 promised is now real.

### GTK → ImGui cross-reference

| `src/dialog.c` | `src/sdl/ui_dialogs.cpp` | Notes |
|---|---|---|
| `dialog_color_setup()` | `ui_dialogs_open_color_setup()` + `draw_color_setup_window()` | Non-modal (every other window this frontend has added is), reused across opens rather than re-created per call |
| `csdialog.color_config` (scratch copy) | `g_cs.scratch` | Same `color_get_config()`/`color_config_destroy()` contract — a real, if minimal, memory-management port, not a shortcut |
| "By node type" notebook page | `draw_color_setup_nodetype_tab()` | One `ColorEdit3` per `NodeType`, bound directly to `RGBcolor::r` (its 3 floats are contiguous, matching `ColorEdit3`'s `float[3]` expectation) |
| "By date/time" page + `gui_spectrum_fill()` | `draw_color_setup_timestamp_tab()` + `draw_spectrum_preview()` | Two `SliderFloat`s ("days ago") replace `GtkDateEdit`, clamped to keep old < new; the spectrum preview is a 32-stop `ImDrawList::AddRectFilledMultiColor` strip sampling the same `color_spectrum_color()` the shipped 1024-stop table uses |
| "By wildcards" page + `csdialog_wpattern_*()` | `draw_color_setup_wpattern_tab()` | Add/remove pattern rows and color groups via ordinary buttons + `InputText`, not a `GtkCList` row model — logic ported (duplicate-pattern check, "only an empty group can be deleted"), not the exact widget shape |
| `csdialog_ok_button_cb()` | The "Apply" button | Commits `color_set_config()` to whichever tab is active, **and calls `color_write_config()`** — see the nvstore finding below |
| `dialog_node_properties()` | `ui_dialogs_open_properties()` + `draw_properties_window()` | Single reusable window (`ImGui::Begin("Properties: <name>###properties_window")` — the `###` pins the window's identity so reopening on a different node doesn't create a second window); fetches `get_node_info()` **once**, at open time, into an owned `std::string` snapshot |
| `look_at_target_node_cb()` | The "Look at target node" button | Same TreeV-collapsed-ancestor eligibility check ported verbatim |
| `dir_contents_list()` (`src/filelist.c`) | The "Contents" tab's table | `filelist.c` is GTK-only and not part of `libfsvcore`; this walks the live `GNode` children directly, the same reason `src/sdl/ui_panels.cpp`'s file list does |

### A real, pre-existing bug this task made observable: `lib/nvstore.c` was a complete stub

Pre-reading `src/color.c` for "where does it persist settings" (the
brief's own instruction) surfaced that `lib/nvstore.c` — the *only*
consumer of which is `color.c`, on *both* frontends — was never
implemented: `nvs_open()` unconditionally returned `NULL`, and every
read/write function was a no-op (`/**** NOTE: ALL THIS HAS YET TO BE
IMPLEMENTED! ****/`). `color_write_config()` therefore wrote nothing,
ever, on either frontend, and `color_read_config()` always fell back to
its hardcoded defaults — this is not an SDL-only gap. This blocks Task
5.3's persistence requirement outright ("relaunch → setting survived"
is impossible if nothing is ever written), which is exactly the
`NEEDS_CONTEXT` trigger the task brief names ("dialog.c's persistence
path can't be reused headless"). Rather than stop, this was scoped and
fixed: `lib/nvstore.c` now implements the full `nvstore.h` contract for
real — an in-memory tree of named nodes (repeated child names are what
a "vector" iterates), serialized to a tab-indented, one-node-per-line
text file at `~/.fsvrc` — with zero new dependencies (plain
malloc/strdup, no glib, matching the file's pre-existing zero-dependency
style) and no change to `nvstore.h`, `color.c`, or `dialog.c`'s call
sites. Verified standalone (a throwaway harness exercising exactly
`color.c`'s nested-group/two-level-vector shape: write, dump, read back,
missing-file defaults, and backslash/tab/newline escaping — all pass)
before ever touching the SDL build. This also benefits the **GTK**
frontend equally — `dialog.c`/`callbacks.c` are untouched, but their own
(dead-code, `#if 0`'d) config path would work too if ever un-commented.

**A second, real pre-existing bug this surfaced**: `color_read_config()`
read the color-mode key back as the literal string `"mode"`, while
`color_write_config()` writes it as `key_color_mode` (`"colormode"`) —
a typo that nvstore's all-stub implementation had silently masked
forever (every `*_default()` read always just returned its default,
key or no key). Fixed in `src/color.c` to read back the same key it
writes, as its own one-line comment there explains — otherwise the
color *mode* specifically would never have survived a relaunch even
with nvstore now real, contradicting this task's own verification bar.

### The "Apply" button calls `color_write_config()` — dialog.c's own OK handler never did

`dialog_color_setup()`'s real `csdialog_ok_button_cb()` only ever calls
`color_set_config()`/`window_set_color_mode()` — nvstore persistence in
the GTK build is dead code, gated behind `callbacks.c`'s `#if 0`'d
"File → Save settings" ("Configuration file not yet implemented").
Task 5.3's brief explicitly requires persistence to survive a relaunch,
so the Apply button here also calls `color_write_config()` — the write
`dialog.c`'s own OK handler always should have made, now that there is
a real store to write to.

### Deliberate deviations

1. **Non-modal**, unlike `dialog.c`'s `gui_window_modalize()`-blocked
   GTK windows — matches every other window this frontend has added
   since Task 5.1 (About, Controls, the dirtree/filelist panel), not a
   GTK-specific idiom this port needs to reproduce.
2. **Color Setup is reused across opens**, not re-created per call like
   `dialog_color_setup()` — `ui_dialogs_open_color_setup()` explicitly
   destroys and refreshes the scratch config on each call instead.
3. **"New color group" always appends** at the end of the group list;
   `dialog.c`'s equivalent can also insert immediately before an
   already-selected group. Dropped: this tab has no
   single-selected-row concept to anchor that on (YAGNI, per the brief).
4. **Properties' "Contents" tab has no icon column** (text `[DIR]`
   prefix instead), matching `ui_panels.cpp`'s own file-list convention
   rather than `dialog.c`'s pixmap.
5. **Wildcard patterns have no edit-in-place.** `dialog.c`'s
   `csdialog_wpattern_edit_cb()` pops up a sub-dialog to edit an
   *existing* pattern's text; `draw_color_setup_wpattern_tab()` only
   offers Remove (delete the row) + Add (append a new one via the
   `InputText`) — editing a pattern means removing it and retyping it.
   Not implemented: no sub-dialog/modal-text-entry mechanism exists
   yet in this frontend, and Remove+Add reaches the same end state.
6. **Properties' Owner/Group drop the numeric uid/gid suffix.**
   `dialog.c` shows `"<name> (uid <n>)"`/`"<name> (gid <n>)"`
   (`dialog_node_properties()`'s own `sprintf()`); `draw_properties_
   window()` shows only the resolved name (`g_props.owner`/`.group`,
   from `get_node_info()`'s `user_name`/`group_name` fields — the raw
   numeric IDs were never carried into the snapshot). A cosmetic
   omission, not a data-availability one: `NODE_DESC(node)->user_id`/
   `group_id` are available at snapshot time in
   `ui_dialogs_open_properties()` if this is ever revisited.

### Verification

- **Both arms build clean from scratch** (`meson setup` + `ninja`, zero
  warnings from any touched file beyond the pre-existing unrelated
  `G_LOG_DOMAIN` redefinition warning in `fsv-scan.c`/`test_scanfs.c`).
  `meson test scanfs` → `1/1 OK` on both (macOS SDL and the Linux GTK
  container, explicit `-Dfrontend=gtk`).
- **Bare `meson setup` on macOS** configures the `sdl` frontend by
  default (`meson introspect --targets` lists `src/sdl/fsv`, no GTK
  executable) — confirms the `meson_options.txt` default flip.
- **Headed, real-`SDL_PushEvent` verification** (temporary
  `FSV_DIALOGS_TEST`-gated harness in `main.cpp` + matching `test_note()`
  instrumentation in `ui_dialogs.cpp`, fully reverted —
  `grep -rn "FSV_DIALOGS_TEST|TEMPORARY" src/sdl/*.cpp` is empty),
  against a fixture directory (`red.c`, `blue.c`, `note.txt`,
  `sub/inner.txt`):
  - Colors → Setup (equivalent call) → real click switches to the "By
    wildcards" tab → real click on "New color group" (creates a group
    with the coded default blue) → real click focuses the pattern
    `InputText`, real `SDL_EVENT_TEXT_INPUT` types `*.c` → real click
    "Add pattern" → real click "Apply": `color_get_mode()` becomes
    `COLOR_BY_WPATTERN`, an offscreen `gpu_screenshot()` before/after
    pair differs (a new color `(0,0,38)` — the shaded rendering of the
    group's `#0000BF` — appears in 448 pixels only in the "after" shot),
    and `~/.fsvrc` (redirected via `$HOME`) shows
    `wpattern.group.color=#0000BF`, `wpattern.group.wp=*.c`.
  - Real click switches to "By date/time", real click "Apply" (using
    the tab's default Rainbow spectrum — see the disclosed gap below):
    `color_get_mode()` becomes `COLOR_BY_TIMESTAMP`, a second
    screenshot differs again (a red tint `(51,0,0)`, consistent with
    the fixture's just-created files landing at the spectrum's "new"
    end), and `~/.fsvrc` shows `colormode=time`.
  - **Persistence, both by file content and by reload**: `cat
    ~/.fsvrc` after the run shows the full nested tree (`colormode`,
    `nodetype.*`, `timestamp.*`, `wpattern.group.*`). A *separate,
    fresh, non-test* process (`--screenshot`, same `$HOME`) re-launched
    against the same fixture reproduces the red-tinted coloring (color
    histogram: `(51,0,0)` at 5026 px, `(253/254,0,0)` at hundreds more)
    — confirms `color_init()` → `color_read_config()` → (via
    `geometry_init()`'s `color_assign_recursive()` call, `geometry.c:
    2682`) actually re-applies the persisted mode at startup, not just
    that the file was written.
  - **Properties**, called directly on `red.c` and on `sub/` (the same
    function the context-menu item calls): logged field values —
    `name=red.c type=Regular file owner=willow group=wheel size=22
    alloc=4,096 mtime=Fri Aug 7 23:11:55 2026` for the file and
    `name=sub type=Directory ... subtree=7` for the directory —
    cross-checked exactly against `stat -f "%z %Sm %u %g"` on the real
    fixture files (`red.c`: size 22, same mtime; `sub`: correctly shows
    *subtree* size 7 — the size of `inner.txt` inside it — not the
    directory inode's own raw size, matching `dialog.c`'s own semantics).
  - Four screenshots (before, after-wildcard, after-timestamp, reload)
    sent to the user for visual review; all four have distinct MD5s.
- **A real bug this harness's construction found and fixed (test
  methodology, not shipped code)**: the very first frame a newly
  selected ImGui `TabItem`/`Button` is drawn, `GetItemRectMin()`/`Max()`
  can read `(0,0)` for one frame before layout settles — a synthetic
  click issued the instant a widget's rect first becomes "found" landed
  at the wrong point. Fixed with a small settle delay (a handful of
  frames, re-reading the rect each time) before trusting it, on top of
  Task 5.2's already-established "separately-timed motion, then
  press, then release" requirement.

### Concerns / disclosed gaps

- **The Gradient spectrum type + its `ColorEdit3`-bound old/new colors
  were not driven through the "By date/time" tab's `Combo` dropdown in
  the automated harness.** An *open* `ImGui::Combo()` popup intercepts
  the very next click as click-away-to-dismiss, which silently ate an
  Apply click in an earlier run of this harness (the mode stayed
  `COLOR_BY_WPATTERN` instead of switching to `COLOR_BY_TIMESTAMP`).
  Selecting a specific dropdown item programmatically would need either
  keyboard nav (this app never enables
  `ImGuiConfigFlags_NavEnableKeyboard`) or per-item rects (the simple
  `Combo()` helper doesn't expose them without rewriting it as
  `BeginCombo`/`Selectable()`, which would mean changing shipped widget
  code just to make it testable). The Apply path itself, the mode
  switch, the nvstore write, and the scene recolor are all still
  verified end-to-end above — just using the timestamp tab's *default*
  spectrum (Rainbow) rather than a freshly-dragged Gradient. The
  `ColorEdit3` widgets themselves (`Older color`/`Newer color`) are
  ordinary, directly-memory-bound Dear ImGui usage, identical in kind to
  every other `ColorEdit3` call in this file that *was* exercised (the
  node-type tab's per-`NodeType` swatches) — not re-verified at the
  picker-popup level, which would be testing Dear ImGui's own widget,
  not this port's logic.
- Same sandbox limitation as Tasks 5.1/5.2: no compositor screen
  capture is available here, so ImGui-overlay pixels (the dialogs
  themselves) are not visually confirmable by screenshot — only via
  internal-state tracing (`color_get_mode()`, the nvstore file, logged
  Properties fields) plus the *scene* recolor, which the offscreen
  `gpu_screenshot()` path does capture (unaffected by ImGui compositing).

## Task 6.1 verification (Xcode project — external build system, unsigned)

`packaging/xcode/fsv.xcodeproj` wraps the Meson build, not replace it.
Its single target, `fsv`, is a **`PBXLegacyTarget`** ("External Build
System" in Xcode's UI) — no compile phases, no source list, no product
Xcode itself signs or links. `buildToolPath` is `/bin/sh`;
`buildArgumentsString` runs `meson setup $BUILDDIR $SRCDIR
--buildtype=$BUILDTYPE` (only if `$BUILDDIR` doesn't already exist —
`meson setup` errors on an existing directory, so idempotency is a
`test -d` guard, not `2>/dev/null || true`) then `ninja -C $BUILDDIR`,
with `$BUILDTYPE` switched on Xcode's `$CONFIGURATION` (`debugoptimized`
for Debug, `release` for Release). `$SRCDIR`/`$BUILDDIR` are derived
from `${PROJECT_DIR}` (an Xcode build setting the tool also exports
into the shell's environment via `passBuildSettingsInEnvironment = 1`).
`objectVersion = 56` (Xcode 14's format) — comfortably below the "46+"
floor the task brief called safe, and this machine's Xcode 26.6 opens
and upgrades it with no prompt.

**No Signing & Capabilities tab exists for this target** — External
Build System targets have no product for Xcode to sign, so "Sign to
Run Locally" isn't a pbxproj setting here at all. The ad-hoc-signing
equivalent lives in `packaging/macos/make-bundle.sh`
(`codesign --force --deep --sign -`), invoked as an explicit,
documented step rather than wired into the Xcode target's build — the
simpler of the two options the brief offered, and it keeps
`make-bundle.sh` useful to people who never open Xcode. `packaging/
macos/Info.plist` sets `LSMinimumSystemVersion` to 14.0, matching the
plan's stated macOS floor (Sonoma). No `fsv.icns` is checked in — the
only icon asset in the tree is `src/xmaps/fsv-icon.xpm`, a legacy GTK
XPM, not a viable `.icns` source without a hand-drawn PNG set; skipped
per the task's own "optional" carve-out and documented in `packaging/
xcode/README.md`. `make-bundle.sh` picks one up automatically if ever
added.

**Bug caught by the first real build attempt:** the initial
`buildArgumentsString` called `meson setup $BUILDDIR` with no explicit
source directory, relying on Xcode's external-tool CWD to be the repo
root. It isn't (`buildWorkingDirectory = ""` does not default to
`$SRCROOT`), so meson received a build directory it couldn't pair with
a source directory it could also find, and failed with `"Neither
source directory '…/builddir-xcode' nor build directory None contain a
build file meson.build."` Fixed by passing `$SRCDIR` (`${PROJECT_DIR}/
../..`) as `meson setup`'s explicit second positional argument, making
the command CWD-independent — the same fix also makes `ninja -C
$BUILDDIR` robust regardless of `buildWorkingDirectory`.

**macOS (this machine, Xcode 26.6 — full IDE, not CLT-only, confirmed
via `xcodebuild -version`):**
- `xcodebuild -list -project packaging/xcode/fsv.xcodeproj` resolves
  the project, lists target `fsv`, configurations `Debug`/`Release`,
  and an auto-created scheme `fsv` (no shared `.xcscheme` was added —
  YAGNI, matches the task's own "or -scheme if you add a shared
  scheme" phrasing as optional).
- Clean-state build: `rm -rf builddir-xcode &&
  xcodebuild -project packaging/xcode/fsv.xcodeproj -target fsv
  -configuration Release build` → **BUILD SUCCEEDED**, ninja runs all
  41 targets, binary lands at `builddir-xcode/src/sdl/fsv`.
- `meson test -C builddir-xcode` → **3/3** (`fsv:scanfs`,
  `fsv:nvstore`, `fsv:color_persistence`) — the same three tests Task
  5.3 added, now proven to also pass through the Xcode-driven build
  directory, not just the hand-run one.
- Idempotency: re-running the identical `xcodebuild … build` command
  logs `ninja: no work to do` (meson setup skipped via the `test -d`
  guard, ninja no-op) — confirms the wrapper doesn't force a
  reconfigure/rebuild on every Xcode build. `-configuration Debug`
  also builds clean (reuses the same `builddir-xcode`, since it
  already exists — switching build type on an existing checkout
  requires deleting `builddir-xcode` first, documented in the README).
- `make-bundle.sh builddir-sdl /tmp/fsv-bundle-test.app` (using an
  existing build's binary): produced a bundle with
  `Contents/MacOS/fsv`, `Contents/Info.plist`, and a valid ad-hoc
  signature (`codesign -dv` → `Signature=adhoc`,
  `TeamIdentifier=not set`, `flags=0x2(adhoc)`). Confirms the default
  `builddir`/`builddir-xcode` lookup logic and the explicit-path
  override both work.
- **Bundle launch check** (the verification bar's substitute for
  `spctl`/Gatekeeper, which won't pass any unsigned bundle regardless
  of ad-hoc signing): running
  `/tmp/fsv-bundle-test.app/Contents/MacOS/fsv --help` printed the
  usage line and exited 0; running it against `tests/fixture` with
  `--screenshot` produced a full Metal-rendered 1280×800 frame
  (`gpu: driver metal` → `gpu: scene pipelines created` → `fsv: wrote
  …ppm (1280x800)`, exit 0) — the bundled binary is not just present
  but fully functional (GPU init, shader load, scene render,
  screenshot) from inside the `.app` layout, which is what "the app
  launches locally" means without a real windowed/Gatekeeper check.
- Both build arms unaffected: `git status --short` after this task
  shows only new files under `packaging/` and the `.gitignore`
  addition — no existing Meson file (`meson.build`,
  `meson_options.txt`, any `src/**/meson.build`) was touched. GTK arm
  (`builddir-gtk`, from earlier tasks) and the default SDL arm both
  still present and unaffected by this task's changes.

`.gitignore` gained `packaging/xcode/build/` (the local build-products
directory `xcodebuild` writes next to the project when not using the
global DerivedData location), Xcode's per-user
`xcuserdata`/`xcworkspace` state, and `/fsv.app` (the bundle
`make-bundle.sh` writes at the repo root by default) — none of these
are build inputs.

## Task 6.2 + 6.3 verification (GitHub Actions CI + release artifacts)

`.github/workflows/ci.yml` — four jobs: `macos-metal`, `linux-gtk`
(anti-regression for the legacy frontend), `linux-sdl` (best-effort),
`release` (tag pushes only). See the comments in the workflow file
itself for the full rationale behind each job; the two decisions worth
calling out here are the ones that changed what gets built, not just
how CI is wired.

**A real, load-bearing build bug found and fixed, not just a CI
config exercise.** Standing up `linux-sdl` locally (an `ubuntu:24.04`
container, mirroring the workflow's exact steps, since GitHub-hosted
runners can't be driven from this sandbox) failed to compile
`src/sdl/gpu.cpp` — first with nonsensical libstdc++ errors deep in
`<vector>` (`'__glibcxx_requires_can_increment_range' was not
declared`), then, once traced further, a `g_list_alloc` macro-arity
clash between `glib.h`'s own declaration and `debug/debug.h`'s macro
of the same name. Root cause, confirmed by preprocessing `gpu.cpp` with
`-E` and reading the resulting include trace: `src/meson.build`'s
`incdir = include_directories('..', '../lib', '.')` had a bare `'..'`
entry. Meson always emits an `-I` flag pair for a relative
`include_directories()` entry — one resolved against the source tree,
one against the build tree — so this single entry meant "expose the
*repo root* on the include path" as a side effect of the *build root*
half (needed to reach `config.h`, generated there by the root
`meson.build`). That's silently harmless for every plain-C compile in
this tree, but libstdc++'s `<bits/stl_algobase.h>` unconditionally
does `#include <debug/debug.h>` (angle brackets) to pull in its own
internal assertion macros — and with the repo root sitting ahead of
`/usr/include/c++/.../debug/debug.h` on the search path (user `-I`
directories always precede a compiler's built-in system dirs, `-isystem`
included), that angle-bracket include silently resolved to *this
project's* `debug/debug.h` instead. This never showed up on
macOS/Clang+libc++ (no colliding header, and/or a different search
order) and never affected the GTK frontend (pure C, never pulls in
libstdc++), which is exactly why four prior tasks' worth of macOS and
Linux(GTK) builds never tripped it — `src/sdl/*.cpp` is the first C++
code in this tree to actually get built on Linux/GCC.

Fix: `config.h` now generates into `<builddir>/src/config.h` (moved the
`configure_file()` call from the root `meson.build` into
`src/meson.build`, keeping the `conf` object itself assembled in the
root file, which subdir-included files can still read) instead of the
build root, reached via the already-present, harmless `-Isrc`/
`-I../src` pair (`src/` has no `debug/` subdirectory of its own to
collide with anything). `incdir`'s bare `'..'` entry is gone.
`debug/meson.build`'s own `static_library()` — which also does
`#include "config.h"` from `debug.c` — picks up the new location via
an explicit `'../src'` added to its own `include_directories:` (kept
its original `'..'` too; harmless for a plain-C compile). Verified with
both an ubuntu:24.04 container (before the fix: `gpu.cpp` fails to
compile as described above; after: `linux-sdl`'s full sequence —
apt install, SDL3-from-source build, `meson setup -Dfrontend=sdl`,
`ninja`, `meson test` — passes end to end, 3/3 tests) and a fresh
`meson setup builddir -Dfrontend=sdl && ninja -C builddir` on this
machine (macOS, unaffected either way, confirming no regression) plus
a `--screenshot` re-run (still a real Metal-rendered BMP).

**SDL3 on Ubuntu, investigated rather than assumed.** `libsdl3-dev`
does not exist in `jammy` (22.04) or `noble` (24.04) — checked via
`packages.ubuntu.com`'s package search across every `noble`/`jammy`
suite variant (base, `-updates`, `-backports`). It first appears in
`questing` (25.10), an interim release GitHub does not offer as a
hosted runner image (only the LTS bases — 22.04/24.04 — are available;
`ubuntu-latest` is 24.04 as of this task). So there is no `apt`
package to install on any GitHub-hosted Ubuntu runner today, on any
release channel. Chosen fix, per the task's own option (a): build SDL3
`3.4.14` (pinned to match the version already verified elsewhere in
this document, Task 2.1's `pkg-config --modversion sdl3` on this
machine) from source via its own supported CMake build, and cache the
*installed* result with `actions/cache`, keyed on
`sdl3-${{ runner.os }}-${{ env.SDL3_VERSION }}` — a version bump is a
one-line `env:` change in the workflow. The exact dependency list in
the `linux-sdl` job's "Install build dependencies" step (X11/Wayland/
EGL/ALSA/PulseAudio/udev/dbus dev headers) is the precise set that
produced a clean `cmake --build` in an `ubuntu:24.04` container for
this task — confirmed with the SDL3 build itself reporting `Video
drivers: dummy kmsdrm(dynamic) offscreen wayland(dynamic) x11(dynamic)`
and `GPU drivers: vulkan` enabled (build-only; no Vulkan *loader* is
needed to run, only to build against the headers). Because this whole
path (an unofficial, from-source SDL3 on Ubuntu) is inherently more
fragile than the apt-packaged GTK path, `linux-sdl` is
`continue-on-error: true` at the job level — it is a bonus build, not
the anti-regression job (`linux-gtk` is). The `release` job's `if:`
condition deliberately does not check `needs.linux-sdl.result`
(and uses `always()` so the *implicit* all-`needs`-must-succeed gate
that GitHub applies underneath any custom `if:` doesn't skip it
whenever the best-effort job fails) — releases still ship the Linux
GTK binary if the SDL one didn't build.

**Screenshot smoke test: real, but honestly unverified from here.**
The `macos-metal` job runs
`./builddir/src/sdl/fsv --screenshot fsv-screenshot.bmp tests/fixture`
as a `continue-on-error: true` step and uploads the resulting BMP only
if it succeeds. This machine's own runs (Task 6.1, this task) confirm
the binary itself does render a real Metal frame headlessly and write
a valid BMP when *this specific piece of Apple Silicon hardware* is
available — but whether a GitHub-hosted `macos-15` runner's
paravirtualized Metal GPU initializes `SDL_GPU`'s Metal backend
headlessly is something this sandbox cannot test (no way to drive a
GitHub-hosted runner from here). The step is written to fail soft
either way; the honest thing to watch on the first real CI run is
whether this step goes green (GPU does init headless on these runners)
or red (it doesn't) — not to assume either outcome from local evidence
alone.

**Runner images:** `macos-15` (Sequoia) rather than `macos-14`
(Sonoma) — checked `actions/runner-images`' own announcements: macOS
14's runner image began deprecating July 6, 2026 and is fully
unsupported November 2, 2026 (i.e., *during* this branch's likely
CI lifetime), while macOS 15 carries no such notice. `ubuntu-24.04`
(noble) for all three Linux-adjacent jobs — the current LTS base,
confirmed still fully supported (22.04's deprecation was announced
separately, unrelated to this choice since 24.04 was already the
target either way).

**Local verification performed for this task** (both jobs' exact
step sequences, not just the workflow YAML):
- `actionlint .github/workflows/ci.yml` (installed via Homebrew): zero
  findings, including its embedded `shellcheck` pass over every `run:`
  block.
- `python3 -c "import yaml; yaml.safe_load(...)"`: parses cleanly.
- **Linux, `linux-gtk`'s exact sequence**, in a fresh `ubuntu:24.04`
  container: `apt-get install` the job's exact package list,
  `meson setup builddir -Dfrontend=gtk`, `ninja -C builddir` (45
  targets, `src/fsv` links), `meson test -C builddir --print-errorlogs`
  → 3/3 OK.
- **Linux, `linux-sdl`'s exact sequence**, same container: the job's
  full dependency list, SDL3 `3.4.14` built from source via the exact
  `cmake`/`ninja` invocations the workflow uses, installed to a
  prefix, `PKG_CONFIG_PATH` pointed at it, `meson setup
  builddir -Dfrontend=sdl`, `ninja -C builddir` (41 targets, `src/sdl/fsv`
  links) `meson test -C builddir --print-errorlogs` → 3/3 OK. This is
  what caught the `debug/debug.h` bug above — it could not have been
  found without actually attempting this exact build.
- **macOS (this machine), fresh builds in scratch directories**
  (`builddir-verify-sdl`, deleted after): `meson setup
  builddir-verify-sdl -Dfrontend=sdl && ninja -C builddir-verify-sdl`
  clean, `meson test` 3/3 OK, and
  `./builddir-verify-sdl/src/sdl/fsv --screenshot ... tests/fixture`
  still writes a real BMP — confirms the `config.h`-relocation fix
  above is a no-op on the platform this port actually targets.
- The `release` job's packaging shell (tarball assembly, including the
  "no SDL artifact → fall back to the GTK binary" branch) was dry-run
  locally against fake artifact directories standing in for
  `actions/download-artifact`'s output layout, for both the
  SDL-present and SDL-absent cases; both produced the expected tarball
  contents.
- Not verified (cannot be, from this sandbox): the actual
  GitHub-hosted runners themselves — the macOS Metal screenshot step's
  real-world outcome, and `gh release create`/`upload`'s live
  behavior. Both are ordinary, well-documented GitHub Actions/`gh` CLI
  operations exercised nowhere unusual; the risk is entirely in the
  *first* real CI run, which is what to watch, not in anything this
  task could further de-risk locally.

## Task 6.2 + 6.3 fix round (code review)

A code review of the first pass above caught three real release-quality
bugs and two minor cleanups, all in `.github/workflows/ci.yml` plus one
supporting script:

**1. The Linux SDL release binary would not have started for anyone
downloading it.** `linux-sdl` built SDL3 as a *shared* library
(`-DSDL_SHARED=ON`) into a CI-local install prefix; `builddir/src/sdl/
fsv` therefore dynamically linked `libSDL3.so.0`, which the `release`
job then happily packaged into the Linux tarball. But per the
SDL3-on-Ubuntu investigation above, there is no `libSDL3` *runtime*
package on any Ubuntu release GitHub hosts either — so a downloader
extracting that tarball would hit `error while loading shared
libraries: libSDL3.so.0: cannot open shared object file`, with no
`apt install` available to fix it themselves (unlike the GTK binary's
system-GTK dependency, or unlike this port's own macOS story, where
`brew install sdl3` is a real, one-command self-service fix). Chosen
fix, the option with the least added machinery: build SDL3 **static**
(`-DSDL_SHARED=OFF -DSDL_STATIC=ON`) instead. No `meson.build` change
was needed — `dependency('sdl3')`'s pkg-config lookup already picks up
whichever of `libSDL3.a`/`libSDL3.so` is actually installed, and SDL3's
own generated `sdl3.pc` folds its (few) required system libs straight
into `Libs:` for a static build, with nothing extra to add. Verified
for this fix round by copying the resulting `fsv` into a **genuinely
bare** `ubuntu:24.04` container (`docker run --rm ubuntu:24.04`, no
packages installed at all beyond the base image): `ldd` shows no
`libsdl3` reference at all (only `libglib-2.0`, `libstdc++`, `libm`,
`libgcc_s`, `libc`); with the base image completely untouched, running
the binary fails on the missing `libglib-2.0.so.0` (glib is *not*
present in Docker's stripped-down base image, unlike a real Ubuntu
install), but installing only `libglib2.0-0` — an ordinary runtime
package, present on virtually any real system already, the same tier
as the GTK binary's system GTK — is enough for `./fsv --help` (which
prints usage via `SDL_Log` and exits 1 before ever calling
`SDL_Init`, so it needs no display) to run to completion. The
workflow's `linux-sdl` job also gained a "Verify no dynamic libSDL3
dependency" step (`ldd | grep -qi libsdl3`, fails loudly if it ever
finds one again) as a standing regression guard, and the SDL3 build
cache key gained a `-static` suffix so no stale shared-lib cache entry
could ever be reused under the same key.

**2. `make-bundle.sh`, bundled into the macOS tarball, could never
succeed against that tarball's own layout.** The script only accepted
a *meson builddir* (looking up `<dir>/src/sdl/fsv` inside it) or fell
back to `<repo_root>/builddir`/`builddir-xcode` — none of which exist
in an extracted release tarball, where the binary sits flat right next
to the script. Fixed by teaching `packaging/macos/make-bundle.sh` to
accept a direct path to an already-built binary as its first argument
(if it names a regular file, it's used as-is; if it names a directory,
the existing builddir lookup runs unchanged) — verified locally against
both forms (`./make-bundle.sh ./fsv fsv.app` and the pre-existing
`./make-bundle.sh <builddir> fsv.app`), both producing a valid
ad-hoc-signed bundle. The macOS tarball also gained a small
`USAGE.txt` spelling out this exact invocation, since neither the
repo's general `README.md` (about building from source, not about a
downloaded tarball) nor anything else in the tarball previously said
so; the Linux tarball got a matching, simpler `USAGE.txt` for
consistency.

**3. Undisclosed deviation from the original brief, now stated
explicitly.** Task 6.3's brief describes release tarballs containing
"shaders/ and README". Neither tarball ships a `shaders/` directory —
correctly so, since Task 3.2 made `tools/embed-shaders.py` embed
compiled shaders directly into the binary at build time, so nothing
under `shaders/` is needed at runtime on either platform. This was
true but unstated in the first pass; now called out in both a
workflow comment (top of `.github/workflows/ci.yml` and again right
above the packaging step) and here.

**4. (minor)** `debug/meson.build` kept a bare `'..'` include entry
"just in case debug.c ever needs another repo-root header", alongside
the `'../src'` entry the config.h fix actually needed. That hedge runs
against the fix's own point (never put the repo root on an include
path unless something actually resolves through it) and, checked
against `debug.c`'s real `#include` list, nothing does. Removed;
`debug.c` now gets only `'../src'`.

**5. (minor)** Inconsistent `set -euo pipefail` usage across the
workflow's `run:` blocks — two `release`-job steps had it, every other
multi-line `run: |` block relied on GitHub Actions' own default
(`bash -e {0}`, i.e. `errexit` only, no `pipefail`, no `nounset`).
Aligned to one style: every multi-line `run: |` block now starts with
`set -euo pipefail` explicitly, rather than leaning on the
runner-shell default. (Genuinely single-line `run:` steps were left
alone — there is nothing for the extra flags to buy on a single
command with no pipe.)

**Verification for this fix round:** `actionlint` + embedded
`shellcheck` clean; `python3 -c "import yaml; yaml.safe_load(...)"`
clean (including confirming the two new tarball-`USAGE.txt` heredocs —
initially written with plain 0-indented body text — actually needed
re-indenting to stay inside the YAML block scalar, caught by a first
failed parse and fixed); the `linux-sdl` job's exact, updated step
sequence (apt install, static SDL3 build, `meson setup
-Dfrontend=sdl`, `ninja`, `meson test`, the new `ldd` verification
step) reproduced end-to-end in a fresh `ubuntu:24.04` container — 3/3
tests, and the verification step confirms zero `libsdl3` in `ldd`;
`linux-gtk`'s sequence re-run unaffected (3/3); a fresh macOS
`meson setup -Dfrontend=sdl && ninja && meson test` (3/3) confirms the
`debug/meson.build` change (item 4) is a no-op there too — this whole
fix round is Linux-CI-only and macOS's own build path was not
otherwise touched; the release-packaging shell re-dry-run against fake
artifact layouts for both the SDL-present and SDL-absent-fallback-to-
GTK cases, now producing tarballs with the new `USAGE.txt` files with
exactly the intended content (confirmed with `tar -xzO`).

## Task 6.4 verification (`--record` demo video)

Added a `--record OUTDIR SECONDS` mode to the SDL frontend and used it to
produce `docs/media/demo.mp4`/`demo.gif`: a scripted camera flythrough of
this repository's own `src/` tree, embedded at the top of `README.md`.

### The real question this task had to answer first

`--screenshot`'s existing offscreen-capture mechanism
(`gpu_screenshot_begin/end()`, Task 3.3) renders the scene into a
private `R8G8B8A8_UNORM` texture and has never carried ImGui — and for
good reason: ImGui's one SDL_GPU pipeline is built once, in `main()`, in
whatever pixel format `SDL_GetGPUSwapchainTextureFormat()` reports (this
port's actual target, confirmed `B8G8R8A8_UNORM`). SDL_GPU pipelines are
format-bound at creation; rendering ImGui's draw data into a
differently-formatted target than the one its pipeline was built for is
not just wrong-looking output, it is an unsupported combination. So a
recording that wants the menu bar and docked panels visible (the brief's
explicit ask — "shows it's a real app") cannot simply reuse
`gpu_screenshot_begin()`'s texture and add an ImGui pass on top.

The fix needs no new ImGui pipeline and no restructuring of the ImGui
backend: `gpu_record_begin()` (`src/sdl/gpu.cpp`) creates its offscreen
texture in the *swapchain's own* pixel format instead of a fixed
`R8G8B8A8_UNORM`, and a new `g_recording_frame` flag tells
`gpu_scene_begin()` to draw into it with pipeline target index 0 — the
same scene/text pipelines a visible frame uses, not `--screenshot`'s
separate index-1 set. Because the format now matches exactly, the
*same* ImGui pipeline built once in `main()` can render straight into
this texture, with a render pass shaped exactly like `submit_frame()`'s
own pass 2 (`LOADOP_LOAD`, no depth target). `gpu_record_end()` then
downloads and writes the frame, decoding whichever of
`R8G8B8A8_UNORM`/`B8G8R8A8_UNORM` the swapchain format turned out to be
(queried live, not assumed) into the matching `SDL_PixelFormat` before
handing the pixels to `SDL_SaveBMP()`.

### The scripted flythrough (`src/sdl/main.cpp`'s `run_record_mode()`)

A small fixed timeline, keyed off wall-clock seconds since recording
started (see below for why wall-clock, not frame count):

1. **0.0s** — the ordinary startup fly-in to root already runs
   automatically (`initial_camera_pan()`, unchanged); nothing to script.
2. **4.6s** — expand `src/sdl/` (`colexp(..., COLEXP_EXPAND)`) and pan
   to it (`camera_look_at_full(..., MORPH_SIGMOID, 2.2)`).
3. **7.2s** — pan to `src/sdl/gpu.cpp` — this port's renderer core, and
   (not coincidentally) the file this task's own investigation spent
   the most time in.
4. **9.8s–11.3s** — a continuous dolly-in, one small `camera_dolly()`
   delta per recorded frame (exactly what a mouse drag sends
   `input.cpp`), not a morph.
5. **11.6s** — switch to TreeV. Naively this would `app_switch_mode()`
   and let the mode's own automatic pan (`camera_treev_lpan_look_at()`,
   scheduled one tick later) fly to wherever `globals.current_node`
   still pointed — `gpu.cpp`, the *previous* cue's target — and then
   need a second, separate `camera_look_at_full()` to actually reach
   this cue's real destination. Two stacked camera cuts, with an
   awkward "camera pointed at nothing while reorienting" gap between
   them (seen and rejected during manual review of an earlier cut of
   the recording — see below). Fixed by setting
   `globals.current_node` to the real target (`src/geometry.c`, the
   largest single file in `src/`) *before* calling `app_switch_mode()`,
   so the mode's own built-in L-pan flies directly to it — one clean
   pan into the new mode.
6. **13.0s–18.5s** — a continuous, gentle `camera_revolve()` around
   `geometry.c` for the remainder of the recording.

Targets are resolved once, up front, via `node_named()` (`common.c`) —
the same absolute-path/component-walk lookup `ui_dialogs.cpp`'s
symlink-target resolution already relies on — against `app_root_dir()`
(wherever `scanfs()` actually `chdir()`'d into), not a hardcoded path. A
target that doesn't resolve (a future checkout with a renamed/missing
file) degrades to "that cue is skipped", not a crash.

### Why the loop paces itself to real wall-clock time

`camera_look_at_full()`/`colexp()`'s morphs time themselves off
`animation.c`'s `xgettime()` — the real wall clock, not "how many times
`fsv_animation_tick()` has been called" (`--screenshot`'s own intro-pan
wait already depends on this same fact). Recording offscreen has no
vsync to wait on, so a naive loop runs many times faster than real
time; calling `fsv_animation_tick()` hundreds of times within a handful
of real milliseconds would let every morph's *progress fraction*
collapse into a tiny slice of real time, then sit frozen for the
(virtual, frame-counted) remainder — a recording that looks like it
jumps, not pans. So each iteration of `run_record_mode()`'s loop
`SDL_Delay()`s until the next 1/30s boundary of real elapsed time
*before* reading `t` and driving that iteration's cues/tick/capture —
making "frame N is video-time N/30s" and "the morph is M seconds into a
2-second pan" the same statement instead of two clocks that can drift
apart.

### `tools/make-demo.sh`

Builds (if needed), records 19s (1s of margin under the 20s cap) into a
scratch directory, then two `ffmpeg` passes: BMPs → `demo.mp4` (h264,
`yuv420p`, `+faststart` — the widely-compatible combination browsers and
GitHub's own README renderer expect) → `demo.gif` (palette-optimized
two-pass, 720px wide, 15fps; automatically retries at 480px/10fps if the
first pass still exceeds the ~10MB budget). Frame dumps are deleted
(`trap ... EXIT`) whether or not encoding succeeds.

### Verification performed

- Both arms: fresh `meson setup builddir -Dfrontend=sdl && ninja &&
  meson test` — 3/3. `-Dfrontend=gtk && ninja && meson test` — 3/3 (on
  macOS this builds the headless core/tests only, unaffected by this
  task; the actual GTK GUI binary is intentionally Linux-only —
  `src/meson.build`'s `host_machine.system() != 'darwin'` gate,
  unrelated to and pre-existing this task — verified Linux-side in
  Task 6.2's CI, not here).
- `./builddir/src/sdl/fsv src --record <dir> 19`: wrote exactly 570
  frames (`19 * 30`), real elapsed time ≈19.4s (pacing overhead from
  `SDL_Delay()`'s granularity, not drift — frame count and content both
  land on the intended timeline).
- `ffprobe` on the resulting `demo.mp4`: `codec_name=h264`,
  `pix_fmt=yuv420p`, `1280x800`, `duration=19.000000`.
- `demo.gif`: 8.6MB (under the 10MB budget, no fallback pass needed).
- **Visual review**: extracted frames at both a dense sampling (during
  development, to catch the two-stacked-camera-cuts problem above) and
  six evenly-spaced timestamps (1s/4s/8s/11s/14s/18s) from the final
  `demo.mp4` and inspected them directly. Confirmed: labels
  (`sdl`/`xmaps`/`geometry.c`/`gpu.cpp`/directory names) readable in
  every sampled frame; the camera visibly at a different position/mode
  in every frame (root overview → MapV `src/sdl/` expanded → `gpu.cpp`
  close-up → TreeV `geometry.c` orbit); the ImGui menu bar and docked
  "Directory Tree" panel (plus its file-list table) visible in every
  frame from ~3s onward; zero black or corrupt frames.
- `actionlint`/CI unaffected — this task touches no workflow files.

### Deviations / notes

- The demo scans `src/`, not the repository root: the root also
  contains `builddir*/`, `.git/`, and other build byproducts that would
  dominate a size-proportional MapV layout (their disk usage dwarfs the
  actual source) and add nothing to a "navigating the source code"
  demo. `src/` is what the brief's own example targets (`sdl` dir,
  `gpu.cpp`) already pointed at.
- `--record` stays in the binary (not a build-time-only tool): it costs
  one `bool` flag, three small functions in `gpu.cpp`, and a
  self-contained block in `main.cpp`; documented above and in this
  file's own "Task 6.4" section for whoever next needs to
  re-record after a UI change.

## Task 6.5 verification (README rewrite + porting retrospective)

Rewrote `README.md` end to end: the metal-port banner now says the port
*is done* (not "is porting"), the Install section is macOS-first (brew
one-liner, `meson setup && ninja`, optional `.app` bundle/Xcode pointer,
prebuilt-binary link) with Linux kept as a secondary section covering
both frontends, a new Controls section replaces nothing (there wasn't
one before) built directly from reading `src/sdl/input.cpp`, and the
entire "TODO" section is replaced by "What's been done" (this port's own
~9 bullets, jabl/fsv's inherited history condensed to 3, and an honest
"Known limitations" list) with the old "Misc notes / OpenGL versions"
section preserved verbatim under a collapsed `<details>` rather than
deleted. Added this retrospective header to `docs/PORTING.md` and
checked off every completed checkbox in the plan doc
(`docs/superpowers/plans/2026-08-06-macos-metal-port.md`), with brief
inline deviation notes at the tasks where the real outcome diverged from
the plan's original sketch (Tasks 3.2–3.4, 4.1, 5.3, 6.1–6.3).

**A real correction, not just a doc pass:** the task brief's own
pre-reading list asserted the controls include "double-click
activate/warp". Reading `src/sdl/input.cpp` in full (its own header
comment, plus the button-down/up handlers) shows this is not the actual
behavior — the port deliberately reproduces `viewport.c`'s exact
original semantics, where a double-click is just two ordinary clicks in
a row (`grep -n "DoubleClick\|clicks ==\|clicks >" src/sdl/*.cpp
src/sdl/*.h` finds nothing; the file's own comment block explains why:
GDK's second `GDK_2BUTTON_PRESS` event added nothing on top of the first
ordinary press, and SDL has no equivalent event to ignore in the first
place). There is no "activate/warp" gesture anywhere in the 3D viewport
in either the original or this port — that behavior belongs to the
separate directory-tree panel (`ui_panels.cpp`, ported from `dirtree.c`
in Task 5.2), not the 3D view. The README's Controls table was written
from the real behavior, and this deviation from the brief's assumption
is called out explicitly rather than silently ported as fact.

**Controls table cross-checked row by row against `input.cpp`:**
left-click (`SDL_EVENT_MOUSE_BUTTON_DOWN`/`UP`, `btn1`) → select on
press (`update_highlight`), fly-to on release
(`camera_look_at(g_indicated_node)`); middle-drag (`btn2` in
`SDL_EVENT_MOUSE_MOTION`) → `camera_dolly()`; Ctrl+left-drag (`ctrl_key
&& btn1`) → `camera_revolve()`; scroll wheel
(`SDL_EVENT_MOUSE_WHEEL`) → `camera_dolly()`, labeled in both the source
comment and the README as an addition, not a port; right-click (`btn3`)
→ `g_context_menu_request` (consumed by `ui_main.cpp`'s context menu:
Look At, Properties…, Expand/Collapse — cross-checked against
`ui_main.cpp`'s `MenuItem` calls); hover with no button
(`SDL_EVENT_MOUSE_MOTION`'s final `else` branch) →
`node_at_cursor()`/`update_highlight()`. Menu highlights table
cross-checked against `ui_main.cpp`'s actual `BeginMenu`/`MenuItem`
calls (File/Vis/View/Colors/Help), not reconstructed from memory.

**Link/anchor verification:** every relative link the new README adds
was confirmed to resolve to a real path in the tree —
`docs/media/demo.gif`, `docs/media/demo.mp4`, `docs/PORTING.md`,
`packaging/macos/make-bundle.sh`, `packaging/xcode/README.md`,
`.github/workflows/ci.yml`, `src/sdl/input.cpp` — all present
(`test -e` on each). The demo embed and its mp4 caption line are
untouched from Task 6.4.

**Install commands run fresh, not just read.** Per this task's own
verification bar, the macOS Install section's exact commands were run
end to end against a clean scratch build directory (outside the repo,
deleted afterward), not merely inspected:
- `brew list --versions` confirmed `glib cglm meson ninja pkgconf sdl3`
  already installed (same one-liner the README and CI both use — no
  version drift to report).
- `meson setup <scratch-builddir> .` configured cleanly (default
  `frontend=sdl`, `Subprojects: imgui: YES`, 13 targets).
- `ninja -C <scratch-builddir>` — **41/41 targets**, zero errors (one
  pre-existing, unrelated `G_LOG_DOMAIN` redefinition warning already
  noted in earlier tasks' verification sections).
- `meson test -C <scratch-builddir>` → **3/3** (`fsv:scanfs`,
  `fsv:nvstore`, `fsv:color_persistence`).
- `./builddir/src/sdl/fsv --screenshot ... tests/fixture` → real Metal
  frame (`gpu: driver metal`, `gpu: depth format D32_FLOAT`, both shader
  pairs loaded, `fsv: wrote ...bmp (1280x800)`), exit 0 — confirms the
  binary the README tells a user to build and run actually works, not
  just that it compiles.
- `./builddir/src/sdl/fsv --help` printed the real usage line.
- `packaging/macos/make-bundle.sh <scratch-builddir> <scratch>.app` —
  produced a valid ad-hoc-signed bundle (`codesign -dv` →
  `Signature=adhoc`), confirming the README's ".app bundle" pointer is
  still accurate after Task 6.2+6.3's fix round changed
  `make-bundle.sh`'s accepted argument shapes.
- Scratch builddir and `.app` deleted after verification, per this
  project's existing per-task convention (never commit scratch build
  output).

Not re-run for this task (no source change, and already covered by
CI/earlier tasks' own verification): the Linux GTK/SDL builds, and a
literal `xcodebuild` invocation (Task 6.1 already proved the Xcode
wrapper builds; this task only changed its README pointer text, not the
project itself).

## Final whole-branch review fix round (2026-08-08)

A review of the branch as a whole (rather than task by task) found five
issues; all five are fixed. Full report, including the AddressSanitizer
output quoted below:
[`.superpowers/sdd/2026-08-06-macos-metal-port/final-fix-report.md`](../.superpowers/sdd/2026-08-06-macos-metal-port/final-fix-report.md).

### Critical: in-flight animations survived Rescan/Change Root — **upstream-PR candidate**

`scanfs()` frees every `NodeDesc`/`DirNodeDesc` and destroys the whole
`GNode` tree when a new root is scanned, but `animation.c`'s two
queues were never purged. Both hold pointers straight into that tree:

- `colexp.c`'s deployment morphs (`src/colexp.c:207`/`:213`) set
  `morph->var = &DIR_NODE_DESC(dnode)->deployment` — i.e. *inside* a
  `DirNodeDesc` — and `morph->data = dnode`. `morph_iteration()` is the
  first thing `fsv_animation_tick()` does, so the very next frame after
  the rescan wrote a `double` into freed memory and then handed the
  freed `dnode` to `colexp_progress_cb()`.
- `camera.c`'s pan morphs end in `pan_end_cb()` (`src/camera.c:957`),
  which schedules `post_pan_end()` with a `GNode *` one frame out; that
  reaches `filelist_show_entry()`, which walks the node's children and
  leaves this frontend's `g_shown_dir` dangling.

Fix: `morph_break_all()` and `scheduled_events_clear()` in
`src/animation.c`, called from `scanfs()` **before** the frees, so the
queues never even briefly hold dangling pointers. Both apply
`morph_break()`'s existing semantics queue-wide — drop the records
without running any step/end callback and without writing through any
morph variable. *Finishing* the queues instead (`morph_finish()`) is
exactly what would fire the dangerous callbacks, so it is not an
option here.

**This is shared core code, in files the GTK frontend also builds
(`animation.c`, `scanfs.c`) — the GTK arm has the same bug and gets the
same fix.** Nothing about it is SDL- or Metal-specific: it is a
straightforward use-after-free in upstream `jabl/fsv`, reachable from
File → Change Root during any camera pan or collapse/expand animation.
Like the `lib/nvstore.c` stub found in Task 5.3, this is a good
candidate to send upstream on its own, independent of the port.

Proven with AddressSanitizer, not just reasoned about — expand a
directory, then Rescan 0.3s later (synthetic menu clicks via
`SDL_PushEvent`, the harness style Task 5.1 established):

```
==40579==ERROR: AddressSanitizer: heap-use-after-free on address 0x61100007dad0
WRITE of size 8 at 0x61100007dad0 thread T0
    #0 morph_iteration animation.c:355        <- *(morph->var) = INTERPOLATE(...)
    #1 fsv_animation_tick animation.c:470
freed by thread T0 here:
    #1 node_data_free scanfs.c:285            <- g_slice_free(DirNodeDesc, p)
    #5 scanfs scanfs.c:328
    #6 load_filesystem(char const*, FsvMode) main.cpp:248
SUMMARY: AddressSanitizer: heap-use-after-free animation.c:355 in morph_iteration
```

The same scenario post-fix, plus three other expand/rescan timings and
the Rescan-during-the-4s-intro-fly-in case, are all ASan-clean.

### Important: SDL frontend state invalidation

`input.cpp`'s `g_indicated_node` (and the pending `ContextMenuRequest`,
which holds the same kind of pointer) survived a rescan, so a
`BUTTON_UP` arriving afterwards fed a freed node to `camera_look_at()`.
New `input_reset()` clears both plus the `SDL_CaptureMouse()` grab;
`load_filesystem()` calls it, and `camera_pan_break()`, before
`scanfs()`. The `camera_pan_break()` call is belt-and-suspenders — the
core's own `morph_break_all()` would take the same morphs out a moment
later — but it is `camera.c`'s documented entry point, and
`morph_break()` is silent on a variable that is not being morphed, so
there is no double-free. It must precede `globals.fsv_mode = FSV_NONE`,
which its `switch` would `SWITCH_FAIL` on.

### Important: hover picks coalesced to one per main-loop iteration

Every no-button mouse-motion event ran `gpu_pick()` — a full offscreen
id-colour render plus a fence wait, 1.85–9.29ms measured in Task 4.2 —
and one `SDL_PollEvent()` drain can hold a dozen motion events. The
motion handler now records the last hover position; `main()` calls the
new `input_flush_hover_pick()` once per iteration, after the drain.
Measured with a 40-event synthetic motion flood in a single drain:

| build | picks in that iteration | resulting highlight |
|---|---|---|
| before | 40 | `…/src/geometry.c` |
| after | **1** | `…/src/geometry.c` |

Same highlight, 40× fewer GPU round-trips. The click path
(`BUTTON_DOWN`) and the "pointless dragging" branch deliberately still
pick inline — see `input.h` and `input.cpp` for why each.

### Important: macOS runtime dependencies documented

`otool -L` on the released binary shows
`/opt/homebrew/opt/sdl3/lib/libSDL3.0.dylib` and
`/opt/homebrew/opt/glib/lib/libglib-2.0.0.dylib`. Neither the tarball
nor `make-bundle.sh`'s `.app` bundles them, but `USAGE.txt` said "run
directly" unqualified. The `requires: brew install glib sdl3` line is
now in the release `USAGE.txt` (`.github/workflows/ci.yml`), README's
Install section and `packaging/xcode/README.md`'s bundle section.
**Actually bundling the dylibs** (`install_name_tool`/`dylibbundler`
into `Contents/Frameworks`) remains deferred, and is the real fix if
this ever needs to be distributed to people who don't have Homebrew.

### Minor: `--record` no longer offers dead menu items

`--record`'s capture loop draws the real menu bar and pumps real
events, but never calls `app_apply_pending_root_change()` — so File →
Rescan/Change Root queued a request that was silently discarded at
exit. They are greyed out during a recording now (new
`app_is_recording()`), the same way they already are during a scan.
Teaching the recording loop to run `scanfs()` mid-capture was rejected:
it would free the tree the recording script's cues hold `GNode *` into.

## Post-port additions

Gestures/features added to the SDL frontend *after* the port itself was
considered done (Task 6.5's retrospective above) — i.e. not present in
upstream `jabl/fsv`, and not something Task 4.1's original gesture port
carried forward either. Kept in their own section, separate from the
task-by-task narrative above, since they are new user-facing behavior
rather than a GTK→SDL translation of existing behavior. The
scroll-wheel dolly (Task 4.1, in the gesture table above) is the same
kind of thing, just labeled inline instead of listed here since it
shipped as part of a task rather than afterward.

- **Double-click a directory to expand/collapse it** (`src/sdl/
  input.cpp`'s `SDL_EVENT_MOUSE_BUTTON_UP` case): releasing the second
  click of a double-click over a directory node now calls `colexp()` —
  `COLEXP_EXPAND` if `dirtree_entry_expanded()` reports it collapsed,
  `COLEXP_COLLAPSE_RECURSIVE` if expanded — instead of the ordinary
  `camera_look_at()`. This is the exact same single-level toggle
  `ui_main.cpp`'s context menu (Expand/Collapse) and `ui_panels.cpp`'s
  directory-tree-panel arrow click already call; no new colexp/dirtree
  entry point was added. The first click of the pair still runs the
  ordinary press/release select-and-fly-to behavior unchanged (so a
  double-click both flies the camera to the directory *and* expands
  it) — only the *second* release is intercepted, and only when the
  indicated node is a directory (`NODE_IS_DIR()`); double-clicking a
  file or empty space is unaffected. Panel sync needs no extra wiring:
  `colexp()` already calls `dirtree_entry_expand()`/
  `dirtree_entry_collapse_recursive()`, which route straight into
  `ui_panels.cpp`'s tree-row state, the same path the panel's own arrow
  click and the context menu already use.
  - Neither `viewport.c` nor Task 4.1's port of it gave the 3D viewport
    any double-click behavior at all — see the (now superseded) gesture
    table row above and this file's own header comment in
    `input.cpp`, which documents both the original no-op and the new
    toggle side by side.
  - GTK frontend (`src/viewport.c`) is untouched — this is an
    SDL-frontend-only addition; `git diff --stat src/viewport.c` against
    this change is empty.
  - Documented in `README.md`'s Controls table and the in-app
    Help → Controls window (`src/sdl/ui_main.cpp`), both marked as an
    addition the same way the scroll-wheel row already is.
  - **Fix round (review, 2026-08-08): a file-double-click regression.**
    `camera_look_at()`'s minimum pan time (`camera.c`'s `*_MIN_PAN_TIME`
    family; DiscV hardcodes 2.0s, `discv_look_at()`) always outlasts a
    physical double-click's inter-click interval, so `camera_moving()`
    is unconditionally still true when the second press arrives. A
    first pass at this feature widened `BUTTON_DOWN`'s pre-existing
    "impatient user" branch (which discards a click that interrupts an
    in-flight pan) to skip the discard for *any* `clicks >= 2` press,
    blind to what was actually under the cursor. That let a
    double-click on a *file* re-arm `BUTTON_UP`'s ordinary
    `camera_look_at()` a second time — and `camera_look_at_full()`
    (`camera.c:974`) is not a no-op for a node that is already
    `globals.current_node`: it unconditionally calls
    `window_set_access(FALSE)` and restarts a full minimum-duration pan
    with zero visible motion, so the second click would have silently
    locked input for up to 2s on every file double-click. Fixed by
    peeking `node_at_cursor()` on the second press *before* deciding
    (only when `camera_moving()` — bounded to the rare case a press
    lands mid-pan, never on an ordinary idle-camera click) and only
    skipping the discard when that peek resolves to a directory
    (`NODE_IS_DIR()`); a file or empty space keeps the exact original
    unconditional discard, so a file double-click now produces exactly
    one `camera_look_at()` call end to end, same as before this feature
    existed.
  - **Disclosed, not fixed — the same `camera_look_at_full()`-on-current-
    node pattern also exists inside `colexp.c` itself**, pre-dating this
    feature: `colexp()`'s `COLEXP_EXPAND` case calls
    `camera_look_at_full(globals.current_node, ...)` whenever
    `curnode_is_equal` (the directory being toggled is already
    `globals.current_node`) — which, for this feature's own most common
    case (double-click flies to the directory on the first press, then
    the second press's `colexp()` call sees that same directory as
    `globals.current_node`), fires a second zero-motion re-pan with the
    same `window_set_access(FALSE)` lock, for the directory's own
    (typically sub-second to 2s) pan duration. This is not new: the
    identical sequence already existed pre-feature via, e.g.,
    right-click → Expand on a directory the camera was just flown to.
    Fixing it means changing `colexp.c` (shared core, also built by the
    GTK frontend) to special-case "already looking at this node", which
    is out of scope for an SDL-frontend gesture addition and was not
    asked for; left as a known pre-existing quirk rather than patched
    incidentally.

- **Escape collapses the current directory** (`src/sdl/input.cpp`'s new
  `SDL_EVENT_KEY_DOWN` case): pressing Escape in the 3D viewport closes
  `globals.current_node`'s directory if it is expanded
  (`colexp(node, COLEXP_COLLAPSE_RECURSIVE)`, the exact call the
  double-click addition above and `ui_main.cpp`'s "Collapse" menu item
  both use) — or, if the current node is a file or an already-collapsed
  directory, collapses its *parent* (only if the parent is itself an
  expanded directory) and flies the camera there
  (`camera_look_at(parent)`), reading as "close and step out". Repeated
  presses walk up the tree one level at a time. `viewport.c` has no
  keyboard handling in the 3D view at all — not even a no-op to
  supersede, unlike the double-click addition's `GDK_2BUTTON_PRESS`
  row above — so this has no gesture-mapping-table row; it is purely a
  new key, not a translation of anything.
  - **Where it terminates.** `root_dnode`'s parent is `globals.fstree`,
    the invisible metanode `scanfs.c` creates with `NODE_METANODE` (not
    `NODE_DIRECTORY`); `NODE_IS_DIR()` on it is false, so the "collapse
    the parent" branch never fires past the real root. Collapsing the
    root directory *itself* is allowed when it is the current node and
    expanded — this matches, not overrides, `ui_panels.cpp`'s tree-row
    arrow, which lets the root row collapse too
    (`draw_dir_node(root_dnode)` is the panel's own top-level call).
  - **A discovery this feature's own verification bar forced**: this
    app's ImGui init (`main.cpp`) never sets
    `ImGuiConfigFlags_NavEnableKeyboard` (only `DockingEnable` is set).
    Reading `imgui.cpp` (not assuming) showed two consequences that
    would otherwise have made this feature silently misbehave:
    - `io.WantCaptureKeyboard` only goes true for an active widget
      (`g.ActiveId != 0`) or a *modal* window (`imgui.cpp`'s
      `UpdateInputEvents()`) — an ordinary `BeginPopup()`, like
      `ui_main.cpp`'s right-click context menu, sets neither. Gating
      this feature on `io.WantCaptureKeyboard` alone would have let
      Escape fall straight through to the collapse logic *while the
      context menu was open* — the opposite of the required "Esc
      closes the menu, scene unchanged" behavior. Fixed by additionally
      checking `ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId |
      ImGuiPopupFlags_AnyPopupLevel)` before acting.
    - ImGui's own built-in Escape-closes-popup behavior
      (`NavUpdateCancelRequest()`) is itself gated on that same
      `NavEnableKeyboard` flag, so — with the flag off — nothing in
      ImGui would have closed that popup on Escape either, leaving it
      stuck open with no way to dismiss it via keyboard at all. Fixed
      in `ui_main.cpp`'s `draw_context_menu()`, inside its own
      `BeginPopup()`/`EndPopup()` scope: an explicit
      `if (ImGui::IsKeyPressed(ImGuiKey_Escape))
      ImGui::CloseCurrentPopup();` — `IsKeyPressed()` works regardless
      of `NavEnableKeyboard`, unlike the nav-cancel path. This runs in
      the same frame `input.cpp`'s own check sees the popup as open, so
      the two never both act on the same keypress: one closes the
      menu, the other declines to touch the scene.
    - Turning on `ImGuiConfigFlags_NavEnableKeyboard` globally instead
      (which would have made both of the above "just work" via ImGui's
      own nav system) was rejected as disproportionate to a single
      keyboard shortcut: it also enables full Tab/arrow-key widget
      navigation throughout every ImGui window in the app, an
      unrelated, unrequested behavior change with its own surface to
      verify.
  - **The same disclosed `colexp()`-already-re-pans quirk applies
    here too**, and for the same underlying reason as the double-click
    addition's `curnode_is_equal` case above: when the current node is
    a descendant of the parent being collapsed, `colexp()` (in
    `colexp.c`) already calls `camera_look_at_full(parent, ...)` on its
    own — but only `if (!camera->manual_control)`. The explicit
    `camera_look_at(parent)` this feature adds right after `colexp()`
    is therefore not always redundant: it is what makes "Esc steps out"
    reliable when the camera *is* under manual control (the user has
    been dragging it), and it is a harmless same-target re-pan when
    `colexp()` already handled it. Not patched inside `colexp.c` itself
    for the same reason given above — shared core, also built by the
    GTK frontend, out of scope for an SDL-only addition.
  - Held-key auto-repeat (`ev->key.repeat`) is explicitly ignored: SDL
    keeps sending `SDL_EVENT_KEY_DOWN` for a held key after the
    platform's repeat delay, and letting that flood into this logic
    would silently walk multiple directory levels up the tree from one
    physical keypress the user never intended to hold for that purpose.
  - **Fix round (review, 2026-08-08): the ImGui gate had two blind
    spots beyond the popup case above.** Both `io.WantCaptureKeyboard`
    and `IsPopupOpen()` only ever see an *active widget*, a *modal*
    window, or an ImGui *popup* -- neither test has any concept of a
    plain `ImGui::Begin()` window, which is exactly what Properties and
    Color Setup (`src/sdl/ui_dialogs.cpp`) both are (this file's own
    header comment: "Neither window is modal... matching every other
    window this frontend has added"). Repro found on review: open
    Properties, don't touch a widget inside it, press Escape -- the
    scene collapsed a node *underneath* the still-open dialog, because
    nothing in the original gate chain knew the dialog was there. Fixed
    by adding `ui_dialogs_handle_escape()` (`src/sdl/ui_dialogs.cpp`/
    `.h`): closes whichever of Properties/Color Setup is open
    (Properties first, then Color Setup, if both happen to be) and
    reports whether it did, checked in this case right after the popup
    checks and before any scene action. A second, narrower gap: a
    right-click and this Escape landing in the *same* `SDL_PollEvent`
    drain (a batched/scripted injection, or just two fast physical
    actions) hit `IsPopupOpen()` *before* the context-menu popup
    exists -- the `BUTTON_DOWN` case only fills
    `input.cpp`'s own `g_context_menu_request` and returns;
    `ui_main.cpp`'s `draw_context_menu()` doesn't call
    `ImGui::OpenPopup()` on it until the *next* frame. Fixed by also
    checking `g_context_menu_request.pending` directly (this file
    already owns that struct) and cancelling the request rather than
    falling through to the scene, so a batched right-click+Escape reads
    as "open the menu, then immediately close it" rather than "open the
    menu AND collapse a node".
  - **Disclosed, not specially handled -- rapid Escape volleys inherit
    the same jerky-camera quirk already disclosed above for the
    double-click addition's `curnode_is_equal` case.** Each Escape that
    lands while a previous one's `colexp()`-driven pan is still in
    flight calls `camera_look_at()`/`colexp()` again immediately
    (`camera_look_at_full()` restarts a fresh minimum-duration pan
    toward the new target rather than letting the in-flight one
    finish), so a fast Esc-Esc-Esc walking up several directory levels
    can visibly reads as several short, jerky re-aims rather than one
    smooth pan up the tree. Verified harmless (no crash, no stuck
    state, no misrouted event -- the second Escape always acts on
    whatever `globals.current_node`/`dirtree_entry_expanded()` state
    the first one already left, since both are plain synchronous C
    calls with no async window between them) but not visually smooth;
    fixing the *camera* side of this would mean changing `camera.c`'s
    pan-restart behavior for every caller, not just this addition, and
    was not asked for here.
  - Documented in `README.md`'s Controls table and the in-app
    Help → Controls window (`src/sdl/ui_main.cpp`), both marked as an
    addition the same way the scroll-wheel and double-click rows
    already are.

- **UTF-8 / accented characters** (`src/fontatlas.c` + `src/fontatlas.h`
  (new), `src/tmaptext.c`, `src/scanfs.c`, `src/common.[ch]`,
  `src/sdl/main.cpp`, `lib/stb_truetype.[ch]` (vendored)). Reported
  against this port: a file named `cosmique français […].mp3` rendered
  on its 3D pedestal as `cosmique franc??ais` — one `?` per non-ASCII
  *byte*. Three separate defects, all fixed here.

  1. **The atlas was ASCII-only.** `tmaptext.c` embedded
     `src/xmaps/charset.xbm`: a baked 512×128 XBM, 96 fixed 16×32 cells,
     codes 32..127, with `get_char_tex_coords()` hardcoding
     `cell = c - 32` and folding everything outside that range to `?`.
     There was no glyph for `é` to find. Replaced by `fontatlas.c`,
     which rasterizes a real monospace TrueType face with stb_truetype
     into the *same fixed-cell layout* — same 16×32 cells, same 32 cells
     per row, so `char_aspect_ratio` (and therefore `geometry.c`'s label
     squeeze/fit math through `get_char_dims()`) is bit-for-bit
     unaffected — over ASCII + Latin-1 Supplement + Latin Extended-A +
     the dash/curly-quote block + `€` (336 cells, a 512×352 atlas). The
     glyph scale is chosen so one character *advance* fits the cell
     width, then clamped so ascender-to-descender fits the cell height;
     glyphs are centered and clipped to their own cell so a tall
     accented capital can never bleed into a neighbor.
     `gpu_text_init()`'s contract (one 8-bit coverage buffer + width +
     height) already took arbitrary dimensions, so **neither backend
     changed**: the GTK/epoxy compat path and the SDL_GPU path upload
     the new atlas exactly as they uploaded the old one.
  2. **Strings were walked by byte.** `text_draw_straight()`/
     `_straight_rotated()`/`_curved()` indexed `text[i]` and used
     `strlen()` as the character count. Now a single shared helper,
     `text_glyph_cells()`, decodes the string *once per draw* into one
     atlas cell per codepoint (`g_utf8_get_char_validated()` /
     `g_utf8_next_char()` — GLib was already a hard dependency), and
     that count is what `get_char_dims()` fits. An uncovered codepoint
     is one `?`, not one per byte; invalid UTF-8 (filenames are
     arbitrary bytes) is consumed one byte at a time as `?` rather than
     walked off the end of the buffer.
  3. **macOS hands back NFD.** HFS+ and APFS return decomposed UTF-8:
     `café.txt` arrives as `cafe` + U+0301 COMBINING ACUTE ACCENT.
     Neither a fixed-cell glyph grid nor ImGui composes combining
     marks, so even with the glyph coverage above it would have
     rendered as `cafe` plus a stray accent cell. `scanfs.c`'s new
     `display_name()` composes to NFC (`g_utf8_normalize()`) and
     sanitizes invalid bytes (`g_utf8_make_valid()`), storing the
     result in a new `NodeDesc::dname` field — interned in the same
     `GStringChunk` the raw name already lives in, so it has the same
     lifetime and costs one free rather than one per node, and
     returning the *same pointer* when the name is already valid NFC
     (the ASCII case, i.e. almost always).

  **Where the normalization happens, and why it is not per frame.**
  `NodeDesc::name` stays byte-exact — every `lstat()`/`chdir()`
  (`node_absname()`), the wildcard matching in `color.c` and every
  sort/compare still use it. `dname` is display-only, read through the
  `NODE_DNAME()` macro (`src/common.h`), and computed exactly once per
  node at scan time. That matters because the label draw path runs
  every frame for every visible node: normalizing there — the obvious
  "display-time" reading — would re-shape every label string 60 times a
  second. Scan time is the cache. The ImGui panels (`ui_panels.cpp`'s
  tree rows and file-list rows, `ui_dialogs.cpp`'s Properties contents)
  read the same cached field, so they cost nothing extra either. The
  one genuinely per-call normalization is `node_absname_display()`
  (`common.c`), used only by the status bar and the context menu's path
  line — short strings, on hover/selection changes.
  - GTK's own panels never needed any of this (Pango composes combining
    marks and renders the raw UTF-8 fine), but `dirtree.c`/`filelist.c`
    were switched to `NODE_DNAME()` anyway: it is also the
    *guaranteed-valid* UTF-8 form, which is what GTK's tree views
    actually require.

  **Font discovery** (`font_atlas_find_font()`, one ordered list, first
  existing file wins, result remembered): macOS
  `/System/Library/Fonts/Supplemental/Courier New.ttf`, then
  `/System/Library/Fonts/Menlo.ttc` (a collection —
  `stbtt_GetFontOffsetForIndex()` picks the face), then
  `Andale Mono.ttf`; Linux DejaVu Sans Mono and Liberation Mono under
  the Debian/Ubuntu, Fedora and Arch paths. If none exists, the atlas
  falls back to the original XBM charset and says so once
  (`g_message()`) — degraded, never fatal, and the old ASCII behavior
  exactly.

  **ImGui panels** got the same defect from the other direction:
  ImGui's built-in ProggyClean is itself a baked ASCII-only bitmap.
  `main.cpp` now loads the *same* discovered face with
  `AddFontFromFileTTF()` (15px, `FontNo` = the .ttc face index), so the
  panels and the 3D labels can never disagree about the typeface. No
  glyph ranges are passed: since 1.92 ImGui loads glyphs on demand when
  the backend advertises `ImGuiBackendFlags_RendererHasTextures`, which
  `imgui_impl_sdlgpu3.cpp` does — so the panels actually cover more than
  the 3D atlas does (they render CJK if the face has it). No font
  found, or the file unreadable (`ImFontFlags_NoLoadError`, so a
  corrupt system font degrades instead of tripping
  `IM_ASSERT_USER_ERROR`) → `AddFontDefault()`, i.e. the ASCII-only
  default font, logged once.

  **stb_truetype is vendored twice, deliberately.** `lib/stb_truetype.h`
  is a verbatim copy of `subprojects/imgui/imstb_truetype.h` (upstream
  v1.26 plus ImGui's warning fixes), compiled by the one-line
  `lib/stb_truetype.c` into `libmisc`. `fontatlas.c` is plain C shared
  by both frontends and must never include an ImGui header; the two
  copies stay independently updatable.

  ### Verification

  - Both arms build clean: `ninja -C builddir-sdl` (macOS/Metal) and a
    `debian:bookworm` container `meson setup -Dfrontend=gtk` + `ninja` —
    zero warnings from any file this change touches, `meson test` 3/3 on
    both.
  - **3D labels, before/after on the same fixture** (`--mapv
    --screenshot`, ten files with accented/CJK/emoji names): before,
    `cosmique fran??ais.mp3`, `caf??.txt`, `??uvre.txt`,
    `nfd_cafe??.txt`, `?????????.txt` (nine `?` for three CJK
    codepoints), `????song.mp3` (four `?` for one emoji). After:
    `cosmique français.mp3`, `café.txt`, `œuvre.txt`, `nfd_café.txt`,
    `???.txt` (three), `?song.mp3` (one). `--treev` shows the same for
    the rotated-leaf and curved-platform label paths
    (`Ångström_ñ_ç.txt`, `Übung_größe.txt`, `dossier_créé`).
  - **The NFD case specifically**: a file created as
    `nfd_cafe\xcc\x81.txt` (verified on disk as `…63 61 66 65 cc 81…`,
    i.e. genuinely decomposed) renders as `nfd_café.txt` in both the 3D
    labels and the panel rows.
  - **Panels**: headed run, screen-captured — tree row `dossier_créé`,
    file-list rows `café.txt`, `cosmique français.mp3`,
    `français école.mp3`, `nfd_café.txt`, `Ångström_ñ_ç.txt`,
    `Übung_größe.txt`, `œuvre.txt`, and `???.txt` / `?song.mp3` for the
    unsupported ones.
  - **Fallback path**, exercised in a container with `/usr/share/fonts`
    removed: logs the "no monospace TrueType font found" message once,
    builds the 512×128 XBM atlas, and maps every non-ASCII codepoint
    (U+00E9, U+0153, U+65E5) to the single `?` cell — the exact
    pre-change behavior.
  - Idle CPU of the headed process unchanged at 0.3–0.5%; the label path
    does no normalization at all (see `dname` above) and one extra small
    allocation per label per draw (the cell array, alongside the vertex
    array `text_draw_*()` already allocated).

## fsn mode

Work recreating the look and feel of the original SGI `fsn` (landscape
sky/ground, wire connectors, spotlight, control rail) on top of the
completed Metal port above. Tracked in
`.superpowers/sdd/2026-08-08-fsn-mode/` (plan + per-task briefs/reports);
this section carries the same per-task verification notes the
milestone-A/B/C task list above does, one `###` per task.

### Task A1 verification (landscape sky/ground presets)

**Files:** `src/fsn-style.h` (new — `FsnLandscape`, `fsn_landscapes[]`,
`FSN_LANDSCAPE_COUNT`, `FSN_LANDSCAPE_OFF`); `src/gpu.h`
(`gpu_set_landscape()` declaration); `src/sdl/gpu.cpp` (real
implementation + `draw_landscape()`); `src/ogl-gpu-compat.c` (no-op);
`src/color.h`/`.c` (`landscape_get()`/`_set()`/`_init()`, nvstore key
`landscape`); `src/sdl/main.cpp` + `src/window.c` (call `landscape_init()`
alongside `color_init()`); `src/sdl/ui_main.cpp` (Display → Landscape
menu); `tools/fsv-headless-stubs.c` (link-only stub, see below).

**Colors: eyeballed, not upstream-verified.** Neither reference
screenshot ships with the repo's own assets — they're the two jpgs named
in the task brief (`3060c037-...jpg`, an overview + main "fsn" window;
`35037135976_...jpg`, inside-a-directory with the spotlight). Both were
sampled pixel-by-pixel with Python/PIL along a geometry-free vertical
strip of the 3D viewport (`x=480,y=252..322` in the first; `x=180,
y=170..238` in the second) to get real RGB values instead of guessing
from a thumbnail. Both read a **vivid, saturated sky blue** (~RGB
2,138,211) at the top of the visible viewport fading to a **pale
cyan-white** (~RGB 140,250,248) at the horizon, over a **medium green**
ground (~RGB 65,140,90) — not the near-black navy top the plan's own
first draft guessed at. That guess undersold the brightness because it
assumed the visible sky band reaches toward the zenith; in both
references the camera is pitched down enough that the visible sky is a
shallow band well above the horizon, nowhere near dark. "night" has no
reference screenshot at all (both captures are daylight) and keeps the
plan's original guess, flagged in `fsn-style.h` for correction once real
reference material turns up. "slate" is not a guess — it is
`src/sdl/gpu.cpp`'s existing Task 2.2 clear color
(`{0.08, 0.10, 0.12}`), copied byte-for-byte so it reproduces today's
look exactly (confirmed below, not just visually).

**Two implementation departures from the brief's own sketch**, both
because the actual `gpu.h` contract doesn't have the primitives the
sketch assumed — documented in full at `draw_landscape()`'s definition
in `src/sdl/gpu.cpp`, summarized here:

1. *"vertex-colored gradient quad"* isn't expressible: `FsvVertex` is
   `{pos, normal}` only, and the scene fragment shader takes its fill
   color from a uniform (`gpu_set_color()`), never a vertex attribute.
   Adding a per-vertex-color path would mean a new vertex format, a new
   pipeline, and a new compiled shader pair (MSL + SPIR-V, the whole
   Task 3.1 offline toolchain) for a two-triangle effect. Instead the
   sky is 32 flat-colored horizontal strips (`SKY_BANDS`), each an
   ordinary `gpu_draw()` call with a CPU-lerped color between
   `sky_top`/`sky_horizon` — smaller surface, no shader work, and the
   banding is not visible in a screenshot at that band count.
2. *"depth-write off"* is `FSV_DEPTH_ALWAYS_NOWRITE` (new `gpu.h` enum
   value), not a per-draw knob on the existing modes — `pipeline_for()`
   always set `enable_depth_write = true` for every (primitive,
   depth-test, target) combination the existing scene pipeline could
   express. **This went through a wrong first attempt, corrected in the
   Task A1 fix round below**: the original version drew the sky through
   an identity projection/modelview at a fixed NDC `z = 0.999`, reasoning
   that every real draw would be nearer and win `FSV_DEPTH_LESS`. That
   reasoning silently assumed linear depth; it is not linear (see the
   fix round). The shipped version disables the depth test outright for
   the sky's draws (`enable_depth_test = false`, which SDL_GPU/Vulkan/
   Metal guarantee also disables the write, regardless of
   `enable_depth_write`), so it can never occlude anything and is never
   occluded by anything drawn before it, independent of the projection's
   shape.

**Ground plane placement.** `geometry_mapv_node_z0()` (`src/geometry.c`)
puts the *bottom* of the root MapV node at world `z=0` and stacks every
directory upward from there (`+z` is world "up" — see
`g_base_modelview`'s own comment in `gpu.cpp`); a ground quad at exactly
`z=0` would sit in the same plane as that bottom face. The ground is
drawn at `z = -6` (`GROUND_Z_OFFSET`) instead — under 5% of
`mapv_leaf_height` (128), so not visually distinguishable at any of
MapV's own scales, and confirmed by screenshot (below) to show no
z-fighting at the box/ground seam. This is one global ground plane for
MapV and TreeV, not yet scoped to `FSV_FSN` (Task B3's job) — **DiscV is
explicitly excluded, not merely undertested; see the Task A1 fix round
below for why an earlier version of this section's claim about DiscV was
wrong.**

**Select-pass discipline.** `gpu_scene_begin()` checks
`g_render_mode == FSV_RENDER_NORMAL` before calling `draw_landscape()` at
all — `g_render_mode` is already correct at that point because
`gpu_pick()` sets `FSV_RENDER_SELECT` *before* it calls
`gpu_scene_begin()`. So in a pick pass neither the sky nor the ground
draws a single pixel, id-colored or otherwise; the select target stays
at its `(0,0,0,0)` clear for any pixel over either one, which
`viewport_node_for_id()` reads back as id 0 ("nothing there") — exactly
like clicking on empty background before this task. Verified directly
(not just by code reading): a temporary debug hook called `gpu_pick()`
at a known-sky pixel and a known-node pixel in the same `--screenshot`
run. DiscV, "classic" landscape on: pick at the top of the frame (pure
sky in the corresponding visible screenshot) → id `0`; pick at the
center (the large disc visible in the same shot) → id `1`. MapV on a
real directory, "classic" landscape on: sky/ground pixel → id `0`; a
node box → id `17`. Re-running the same two picks with "slate" instead
of "classic" returned the identical ids, confirming the landscape
preset has zero effect on picking, as intended. The hook was removed
before committing — it is not part of this task's shipped diff.

**Persistence.** Same nvstore pattern `color_write_config()`/
`color_read_config()` already established, alongside it in
`src/color.c`: `landscape_set()` calls `gpu_set_landscape()` immediately
and writes the `landscape` key (int token by preset name — `"classic"` /
`"night"` / `"slate"`) via `nvs_write_int_token()`; `landscape_init()`
(called once at startup, right after `color_init()`, in both
`src/sdl/main.cpp` and `src/window.c`) reads it back the same way
`color_init()`/`color_read_config()` do, defaulting to `"slate"` (index
2) when the key is absent — i.e. every `~/.fsvrc` written before this
task keeps today's exact look with no migration needed. Verified with a
standalone harness linked against `libfsvcore` the same way
`tests/test_color_persistence.c` is (not added to `tests/meson.build` —
the task's own verification bar pins the suite at "3/3", so this stayed
a one-off, not a fourth test): `landscape_init()` with no config file →
`landscape_get() == 2`; `landscape_set(0)` → `~/.fsvrc` gains a
plain-text `landscape classic` line; a second `landscape_init()` call in
the same process (the same "simulate a relaunch" trick
`test_color_persistence.c` uses) → `landscape_get() == 0`. Confirms both
directions: on-disk format and reload.

**Headless-link fix required.** `src/color.c` is part of `libfsvcore`
(built for both frontends' unit tests and `tools/fsv-scan`, with no GTK
or SDL_GPU present), and now calls `gpu_set_landscape()` — a symbol
neither `src/sdl/gpu.cpp` nor `src/ogl-gpu-compat.c` contributes to that
build. `tools/fsv-headless-stubs.c` gained a one-line no-op stub for it,
the same shape as its existing `window_set_color_mode()` stub; without
this, `tests/test_scanfs`, `tests/test_color_persistence` and
`tools/fsv-scan` would all fail to link the moment `color.c` grew the
new call. Caught by actually rebuilding and running `meson test` rather
than assuming the change was additive.

**Screenshot verification** (`tests/fixture` and a real directory —
this repo's own `src/`), all read back with PIL, not just eyeballed in a
terminal:

- **Sky gradient**: `--discv` on `src/` gives a full-frame, clean
  vivid-blue-to-pale-cyan gradient with no visible banding at 32 bands,
  and (post-fix-round) no ground wall behind it — see the fix round for
  what DiscV's camera actually does and why an earlier draft of this
  note mischaracterized it as "phi=0, a level camera".
- **Ground + no z-fighting**: `--mapv` and `--treev` on `tests/fixture`
  and on `src/` both render solid, correctly-colored green ground filling
  the frame around the scene geometry; a 4x pixel-zoomed crop of the
  MapV box/ground seam and the TreeV platform/ground seam both show a
  clean edge, no mottling/dithering artifact.
- **Combined horizon shot**: MapV's and TreeV's *resting* default camera
  elevation (`mapv_camera_phi()` returns a fixed 52.5°; TreeV's
  equivalent settles at 30°) points the *entire* vertical FOV below
  horizontal at this app's fixed 60° FOV/1280×800 aspect, so neither
  mode's out-of-the-box `--screenshot` ever shows both sky and ground in
  the same frame — this is a pre-existing, fixed camera characteristic
  of this port (there is no "front view" camera reset implemented at
  all, dormant or otherwise), not something this task changed. Confirmed
  the sky+ground+horizon composition the reference screenshots show by
  temporarily forcing a shallower elevation (`camera->phi = 15`,
  test-only, not shipped): the resulting shot shows a crisp horizon line
  with the vivid-blue-to-pale sky above and green ground below for
  "classic", the same composition darker for "night", and no visible
  horizon at all for "slate" (uniform flat color, as intended).
- **"slate" byte-exact regression check**: stashed this task's changes,
  rebuilt, took a `--mapv` screenshot of `tests/fixture` with no
  `~/.fsvrc` present (pre-A1 binary); restored the changes, rebuilt, took
  the same screenshot with the default (`landscape` key absent → "slate")
  config. Diffed the two BMPs with PIL/numpy: **zero differing pixels**.
  "slate" is not merely similar to the pre-change look, it is pixel-for-
  pixel identical.

**Both arms build clean.** SDL/macOS: `ninja -C builddir-sdl`, 3/3 tests.
GTK: fresh `debian:bookworm` container, the CI job's own package list
(`meson ninja-build libglib2.0-dev libcglm-dev libgtk-3-dev libepoxy-dev
gettext file libglu1-mesa-dev`), `meson setup -Dfrontend=gtk`, `ninja`
(47/47 targets, `src/fsv` links, only pre-existing unrelated
`G_LOG_DOMAIN` redefinition warnings), `meson test` → 3/3.

### Task A2 verification (7-bucket age spectrum + ages legend)

**Files:** `src/color.h` (`SPECTRUM_FSN_BUCKETS`, inserted before
`SPECTRUM_NONE`; `color_timestamp_spectrum_type()` accessor);
`src/color.c` (`fsn_bucket_color()`, the `time_color()`/
`color_spectrum_color()` branches, `tokens_timestamp_spectrum_type[]`'s
new `"fsnbuckets"` token); `src/fsn-style.h` (`FsnAgeBucket`,
`fsn_age_buckets[7]`, `FSN_AGE_BUCKET_COUNT`); `src/sdl/ui_rail.cpp`/`.h`
(new — `ui_legend_draw()`), registered in `src/sdl/meson.build`;
`src/sdl/main.cpp` (calls `ui_legend_draw()` alongside
`ui_dialogs_draw()`, both loop sites); `src/sdl/ui_dialogs.cpp` (Color
Setup timestamp tab's spectrum `Combo` gains a fourth choice);
`src/dialog.c` (GTK's own combo gains the same fourth choice — trivial,
see below); `tests/test_color_persistence.c` (extended).

**Enum insertion safety, actually checked, not assumed.** The brief
asked to verify that inserting `SPECTRUM_FSN_BUCKETS` before
`SPECTRUM_NONE` is safe on disk. It is: `lib/nvstore.c`'s
`nvs_write_int_token()`/`nvs_read_int_token_default()` persist a
`SpectrumType` as the matching **string** in
`tokens_timestamp_spectrum_type[]` (`"rainbow"`/`"heat"`/`"gradient"`),
never the raw enum integer — confirmed by reading both functions, not
just inferred from the array's shape. An existing `~/.fsvrc` with (say)
`spectrumtype gradient` resolves by string match regardless of where
`SPECTRUM_FSN_BUCKETS` lands numerically; the new `"fsnbuckets"` token
was added at the matching array position (index 3, same as the
enumerator's position), the same hand-kept-in-sync convention every
other token array in `color.c` already relies on.

**Colors: sampled, not eyeballed — and the brief's own guess for the
7th bucket was wrong.** The reference screenshot's "ages:" bar
(`35037135976_0d90f4a3d5_z.jpg`, approx. `x=148..284, y=497..506`) was
cropped and, for each of the 7 swatches, given a per-channel **median**
pixel value in Python/PIL (robust against the bold white label text and
JPEG ringing sitting on top of each flat fill) — the same
sample-real-pixels approach Task A1 used for the sky/ground colors. Six
of the seven read cleanly as distinct hues (maroon-red, orange-brown,
olive-yellow, dark teal-green, deep blue, purple). The 7th ("> 1 yr"),
which the brief's own first-pass description called "grey", does not
survive a close look: median ≈ `(87, 44, 68)`, and an 8x crop of just
that swatch (checked directly, not inferred) shows a visibly dark
plum/wine color with a clear reddish-purple hue, not a neutral
grey — corrected in `fsn-style.h` to the sampled value, with the
before/after reasoning documented right next to the table.

**Bucket semantics.** `fsn_bucket_color()` (`src/color.c`) steps a
file's age (`now - node's chosen timestamp`, real wall-clock `time(
NULL)`, computed inside `time_color()`) through `fsn_age_buckets[]`'s
`max_age_s` cutoffs (7d/14d/30d/91d/182d/365d, catch-all beyond) and
returns that bucket's color; deliberately **ignores**
`color_config.by_timestamp.old_time`/`new_time` — fsn's buckets are
fixed, absolute ages, not the continuous, user-adjustable windowed
spectrum the rainbow/heat/gradient path uses. Directories are
unaffected: `time_color()`'s existing `NODE_IS_DIR` early-return (node
type color, not a timestamp color) sits before the new branch and was
not touched. `color_spectrum_color()` also gained a
`SPECTRUM_FSN_BUCKETS` case — not for real node coloring (that always
goes through `fsn_bucket_color()` instead), but because
`generate_spectrum_colors()`'s 1024-shade table build and
`ui_dialogs.cpp`'s Color Setup preview strip both unconditionally
sample `color_spectrum_color()` across `x=[0,1]` and would otherwise hit
`SWITCH_FAIL`; the added case steps through the 7 bucket colors in `x`
order, which incidentally makes the preview strip show a legible
7-band swatch of the bucket palette.

**Select-pass discipline unaffected.** `node_set_color()`
(`src/geometry.c`) already branches on `gpu_render_mode()` before ever
reading `NODE_DESC(node)->color` — in `FSV_RENDER_SELECT` it paints the
node's id-derived color unconditionally and never looks at the color
this task's spectrum choice feeds into `color_assign_recursive()`. So
`SPECTRUM_FSN_BUCKETS` changes only what `NORMAL`-pass pixels show, with
zero effect on picking; verified by reading the function (unchanged by
this task) rather than by re-running the Task A1 pick-hook exercise,
since nothing this task touched is anywhere near that code path.

**The legend bar and the `--screenshot`/ImGui gap.** `--screenshot`'s
offscreen capture (`gpu_screenshot_begin/end`) renders only the 3D scene
pass and has never composited ImGui (Task 6.4's own investigation, cited
in this file's `--record` section, established why: the ImGui pipeline
is built once against the swapchain's own pixel format, and
`--screenshot`'s texture uses a different, fixed one). `ui_legend_draw()`
is a plain ImGui window, so proving it draws correctly needed the one
mechanism in this codebase that *does* composite ImGui into a real,
GPU-rendered frame without a visible display: `--record`. Used
end-to-end for this task's headed verification rather than a live
screen capture (this sandbox has no display to capture from — a gap
already disclosed in the Task 5.2 fix-round section above):

1. A throwaway harness (same shape as `tests/test_color_persistence.c`
   — `color_init()`, `color_get_config()`, flip `spectrum_type`,
   `color_set_config()`+`color_write_config()`, never committed to the
   repo) wrote a real `~/.fsvrc` with `colormode time` /
   `spectrumtype fsnbuckets` under a scratch `$HOME`.
2. A fixture directory got 7 files, each `touch -t`-ed to a distinct
   age (3d/10d/20d/60d/120d/200d/500d — one per bucket, all with
   non-trivial size so MapV's layout gives each a real, distinct box
   instead of degenerating into a zero-area sliver) and non-zero
   content.
3. `fsv --record OUTDIR SECONDS FIXTURE_DIR` with that `$HOME` produced
   30fps BMP frames; `--record`'s scripted camera cues target this
   repo's own `src/` paths and log-and-skip when they don't resolve
   under the fixture root (by design, per Task 6.4), so this just
   recorded the ordinary automatic fly-to-root pan with no crash.
4. The final frame (read back with PIL, not eyeballed) shows: the
   bottom-center "ages:" bar with all 7 correctly-labeled, correctly-
   colored swatches in the reference's own order, **and** all 7 fixture
   files rendered as 7 visibly distinct box colors. Per-pixel sampling
   of each box's lit top face confirms exact correspondence to
   `fsn_age_buckets[]`: every one of the 7 sampled colors is that
   bucket's defined RGB scaled by the same ~0.80 lighting factor (e.g.
   "1 wk" red `(144,60,53)` defined → `(116,48,43)` sampled, ratio
   0.80/0.80/0.81; "> 1 yr" plum `(87,44,68)` defined → `(70,36,55)`
   sampled at its lit face, ratio 0.80/0.82/0.81) — the same uniform
   attenuation across all 7, which is exactly what one shared diffuse
   lighting term applied to 7 different flat input colors should look
   like, not 7 independent coincidences.
5. **Toggling to rainbow hides the legend**: re-wrote the same
   `~/.fsvrc` with `spectrumtype rainbow` (mode left at
   `COLOR_BY_TIMESTAMP`) and re-ran `--record` on the same fixture — the
   resulting frame shows the expected continuous rainbow coloring on the
   3D nodes and **no legend bar at all**, confirming
   `ui_legend_draw()`'s `color_get_mode()`/
   `color_timestamp_spectrum_type()` gate actually gates.
6. Color Setup dialog round-trip: covered structurally (the "fsn
   buckets" `Combo` entry feeds the same, unchanged
   `color_set_config()`/`color_write_config()`/Apply-button path every
   other spectrum choice already uses) and end-to-end by
   `tests/test_color_persistence.c`'s new assertions (below) — not
   separately re-verified by driving the live ImGui widget, since
   nothing about *how* the widget commits its value changed, only which
   values it offers.

**TDD.** Extended `tests/test_color_persistence.c` with a `set
SPECTRUM_FSN_BUCKETS → color_write_config() → color_init() (simulated
relaunch) → assert` block, same shape as the file's existing
by_wpattern regression case. Confirmed RED first — not just by
inspection — via `git stash push -- src/color.h src/color.c` (reverting
only the enum/logic, keeping the new test), which failed to compile
with `use of undeclared identifier 'SPECTRUM_FSN_BUCKETS'`; `git stash
pop` restored the implementation and the suite went GREEN.

**GTK arm.** `color.c` is shared, so GTK gets the 7 bucket colors for
real node coloring for free. `src/dialog.c`'s own spectrum combo (a
plain `GtkComboBoxText`, not a structural widget) needed only a fourth
`gtk_combo_box_text_append_text()` string and one more `strcmp()`
branch in its "changed" callback — genuinely trivial, done rather than
skipped. The ages legend bar itself is **not** ported to GTK: it is a
plain ImGui window with no GTK equivalent, and the plan scopes it as
SDL-only. Verified in a fresh `debian:bookworm` container (this port's
macOS host has no GTK, matching CI, same package list as Task A1's own
GTK verification): the real `fsv` executable — including `dialog.c` —
builds clean, `meson test` → 3/3.

**Both arms build clean, tests 3/3.** SDL/macOS: `ninja -C
builddir-sdl`, `meson test -C builddir-sdl` → 3/3 (including the new
bucket-persistence assertions inside `test_color_persistence`). GTK:
`debian:bookworm` container as above, 3/3 (this arm's test binaries link
`libfsvcore` and don't depend on `gtkdep`, so the same 3 tests run
regardless of whether the full GTK `fsv` executable also happens to
build on that host — it did here).

### Task A3 verification (fsn-style camera control rail)

**Files:** `src/window.h` (`window_access_enabled()`, `window_birdseye_active()`/
`window_birdseye_set_active()` — SDL-frontend-only accessors, doc-commented as
such); `src/sdl/stubs.c` (real storage behind `window_set_access()`/
`window_birdseye_view_off()`, previously no-ops); `src/sdl/app.h`/`main.cpp`
(`app_reset_camera()`; `app_get_scroll_range()`/`app_scrollbar_dragged()`;
`ScrollAxisState` replaces the old value-only `g_scroll[2]` so lower/upper/page
survive `fsv_platform.set_scroll()` instead of being discarded; `app_switch_mode()`
refactored to share a `run_mode_entry()` helper with the new `app_reset_camera()`);
`src/sdl/ui_rail.h`/`.cpp` (`ui_rail_draw()`, `ui_rail_get_visible()`/
`ui_rail_set_visible()` — extends the file Task A2 started); `src/sdl/ui_main.cpp`
(View menu gains "Camera Rail", same toggle pattern as "Directory Tree && Files").

**Pre-reading, as the brief required.** Read `camera_look_at_previous()`
(history is a `GList` of previously-visited nodes; backtracking marks the
head `NULL` rather than popping, so `camera_look_at_full()` can tell "went
back" apart from "went forward" and skip re-pushing); `camera_birdseye_view(
boolean going_up)` (`going_up` TRUE enters the overhead pose and snapshots
the pre-birdseye camera into a static `union AnyCamera`; FALSE morphs back to
that snapshot — confirmed `camera_init()` never itself clears
`birdseye_view_active`, see the Reset note below); `camera_scrollbar_moved(
axis)` and the `ScrollState`/`*_get_scrollbar_state()` family (`mapv_
get_scrollbar_state()`/`treev_get_scrollbar_state()` are real; `discv_
get_scrollbar_state()` is a `/* TODO */` stub that just echoes back whatever
`scroll_state[]` already held); `src/window.c`'s GTK impl (`gtk_set_scroll()`/
`gtk_get_scroll()`/`on_scrollbar_value_changed()`) to mirror its contract:
the camera *pushes* lower/upper/page/value via `set_scroll()`, and a user
drag both updates the widget's own value *and* triggers `camera_scrollbar_
moved()`, which reads that new value back via `get_scroll()`. This port's
`sdl_set_scroll()` used to discard lower/upper/page (`(void)`-cast) since
nothing read them back; they are now kept in a small `ScrollAxisState`
struct so the rail's sliders can render a real range.

**Axis-to-label mapping, verified not guessed.** `task-A3-brief.md` asked to
confirm what each scrollbar axis actually drives per mode before slapping
"Tilt"/"Height" on them. Read `mapv_scrollbar_move()`/`treev_scrollbar_move()`
directly: in MapV, axis 0 (X) pans `target.x` sideways and yaws `theta` to
compensate; axis 1 (Y) pans `target.y` forward/back *and* pitches `phi` — the
felt effect of axis 1 is closer to "flying up/down over the map" than axis 0
is, so the upstream "Tilt"/"Height" labels land closer to right for Y than
X. In TreeV, axis 0 rotates `target.theta` around the tree's own axis (a
spin, not a pan) and axis 1 moves `target.r` (radial distance — closer to a
dolly than a height change). Kept the upstream labels for continuity with
the reference screenshot rather than inventing new ones, and documented the
imperfect fit in `ui_rail.cpp`'s file header rather than silently relabeling,
per the brief.

**Reset semantics.** `app_switch_mode()`'s body (`geometry_init()` +
`camera_init(mode, FALSE)` + a short `""`-message intro pan) was extracted
into `run_mode_entry()` so `app_reset_camera()` can call the identical
sequence for the *current* mode — `app_switch_mode()`'s own `mode ==
globals.fsv_mode` guard exists specifically to reject exactly what Reset
needs to do. One addition beyond a bare `run_mode_entry()` call: if
bird's-eye view is active, `app_reset_camera()` first calls
`camera_birdseye_view(FALSE)` and `window_birdseye_set_active(FALSE)` — read
in isolation, `camera_init()` never touches `birdseye_view_active`, so a
Reset while airborne would otherwise leave that static flag TRUE behind a
non-birdseye camera pose, desyncing the rail's own "Birds eye" toggle
highlight and any *subsequent* toggle click's `going_up` polarity from
reality. No new camera math — both calls already exist in `camera.h`.

**`window_set_access`/`window_birdseye_view_off` are no longer no-ops.**
`src/sdl/stubs.c` now stores what real widgets would otherwise carry:
`window_set_access(enabled)` sets a flag `window_access_enabled()` reads
back (buttons/sliders wrap `ImGui::BeginDisabled(!access_ok)`, `access_ok =
!app_is_scanning() && window_access_enabled()`); `window_birdseye_view_off()`
(the core's "you were in bird's-eye view, you no longer are" notification —
fires from `camera_look_at_full()` when the user picks a new node while
airborne) clears a second flag `window_birdseye_active()` reads for the
"Birds eye" button's highlighted/pressed look, kept in sync from *both*
directions (this button's own clicks, and the core silently exiting
bird's-eye view on its own).

**Sliders gated to MapV/TreeV, per the brief.** `discv_get_scrollbar_state()`
being a stub with no real per-node math is why DiscV's Tilt/Height sliders
are disabled with a "MapV/TreeV only" label rather than merely displaying a
meaningless range — `sliders_ok = access_ok && (mode == FSV_MAPV || mode ==
FSV_TREEV)` gates the pair independent of the buttons' own `access_ok`.

**"Front view" is the brief's own named approximation, not a port.**
`camera.h` has no pose-specific front-view entry point (upstream fsn's exact
pose is not reproduced — no new camera math per the plan), so this button
re-runs `camera_look_at()` on `globals.current_node`, documented inline as
an approximation.

**Headed, direct-state verification (not pixel-accurate ImGui click
simulation).** Following the brief's explicit "SDL_PushEvent / direct state
where cleaner" allowance: simulating a mouse click at a button's exact
screen rect would test ImGui's own hit-testing, not this task's wiring. A
temporary, never-committed set of `--record` cues (same throwaway-harness
convention Task A2 used) called the exact app.h/camera.h entry points each
rail control invokes, gated behind an env var, with `SDL_Log` proof of the
resulting state, then was deleted before the commit below. On `tests/
fixture` in `--mapv`:
- **Go back after two look_ats**: `camera_look_at(dir-a)` then `camera_look_at(
  dir-a/dir-b)` (current node confirmed via pointer identity at each step),
  then `camera_look_at_previous()` — logged `current_node` back to `dir-a`'s
  pointer, matching `camera_look_at_full()`'s backtracking-vs-forward logic
  read during pre-work.
- **Birds eye**: `camera_birdseye_view(TRUE)` — `window_access_enabled()`
  flips to 0 in the very same call (`window_set_access(FALSE)` is the second
  statement of `camera_birdseye_view()`); ~4s later (the fixed `MAPV_CAMERA_
  MAX_PAN_TIME`), settled state logged `phi=90.00` (the overhead pose) and
  `window_access_enabled()` back to 1 (`post_pan_end()`'s scheduled
  re-enable). `camera_birdseye_view(FALSE)` afterward restored the
  pre-birdseye `phi`/`theta`/`distance`.
- **Reset**: `app_reset_camera()` logged an *immediate* jump to `camera_init()`'s
  fresh MapV pose (`theta=270 phi=90 distance=1728 target=(0,0)`), settling
  ~1s later into the same intro-pan-derived framing `enter_mode()` itself
  produces.
- **Sliders in MapV**: `app_get_scroll_range()`/`app_scrollbar_dragged()` on
  both axes, mid-session (with `dir-a` already expanded from the go-back
  test above): axis 0 drag moved `target.x` 6.21 → 4.65 and `theta` 279.60 →
  277.20; axis 1 drag moved `target.y` 3.12 → 0.78 and `phi` 54.43 → 52.98 —
  both instantaneous (no morph involved in `mapv_scrollbar_move()`), matching
  the direct assignment read during pre-work.
- **Access flag during a pan, checked at every rail-drawn frame, not just
  cue boundaries**: a second temporary probe logged `window_access_enabled()`
  and `camera_moving()` together on a frame-sampled cadence throughout a
  birds-eye pan and confirmed `access=0`/`moving=1` for the pan's full
  duration, `access=1` only once `camera_moving()` had already gone false.

**Visual dimming: a documented gap, not a false claim.** `ImGui::
BeginDisabled(!access_ok)` wraps the four buttons exactly like `ui_panels.cpp`'s
own established use of the same call (its file-list table), and a forced
`PushStyleVar(ImGuiStyleVar_Alpha, 0.05f)` sanity check confirmed this
backend's alpha blending itself works (pixels dropped to near-black at that
extreme). But a paired-frame screenshot comparison at the *default*
`DisabledAlpha` (0.60) between a confirmed-idle and a confirmed-disabled
moment showed only a 1–2/255 per-channel difference — likely swamped by
ImGui's own per-window focus-alpha variance in this floating (non-docked)
window, which shifted brightness by more than that between otherwise
identical idle frames in the same test. The flag and the `BeginDisabled`
call are both proven correct by direct-state logging (above); the *visual*
greying is real per ImGui's documented contract but was not independently,
unambiguously confirmed by pixel inspection. Flagged here rather than
asserted as seen. The DiscV sliders' "MapV/TreeV only" label is unambiguous
in a screenshot regardless (see below) and is not affected by this caveat.

**Screenshots** (`--record` on `tests/fixture`, ImGui composited for real —
`--screenshot` does not composite ImGui, per Task 6.4's own note): MapV, rail
idle — left-anchored "Camera Rail" window with Reset/Go back/Birds eye/Front
view stacked buttons, a separator, then Tilt/Height vertical sliders
side-by-side, matching the reference image's left-rail layout (it overlaps
the Directory Tree panel's own default top-left placement until a user
drags either window — both default to the same corner; a cosmetic overlap
this task did not attempt to resolve, since neither panel is force-docked
into a fixed split, see `ui_rail.h`'s "ordinary dockable window" design
note). DiscV, same rail: Tilt/Height sliders visibly inert with a
"MapV/TreeV only" label directly beneath them, buttons still active (Reset/
Go back/Front view/Birds eye all make sense in DiscV, only scrollbar-driven
panning does not).

**Both arms build clean, tests 3/3.** SDL/macOS: `ninja -C builddir-sdl`
clean, `meson test -C builddir-sdl` → 3/3. GTK: fresh `debian:bookworm`
container, CI's own apt list — `window.c`'s `set_scroll`/`get_scroll`
contract (`gtk_set_scroll()`/`gtk_get_scroll()`/`on_scrollbar_value_changed()`)
is untouched by this task, confirmed by re-reading it during pre-work rather
than assumed — real `fsv` executable (47/47 targets) builds clean, `meson
test` → 3/3.

**Rail visibility.** View menu gains "Camera Rail" (`ui_rail_get_visible()`/
`ui_rail_set_visible()`), same toggle pattern as "Directory Tree && Files".
`ui_rail_draw()` returns early during `g_scanning`/`FSV_NONE`/no-tree, same
three guards `ui_panels_draw()` already uses for the Directory Tree panel,
so the rail is hidden outright (not merely greyed) during a scan.

### Task A1 fix round (code review)

A from-source-trace review of the verification above found two real bugs
in the first version of this task and one mischaracterization in this
file's own wording. All three are fixed in the same commit series; this
subsection is the honest record of what was wrong and why, left in place
rather than silently rewriting the sections above.

**Critical — the "DiscV's ground does not render at all" claim was
false**, confirmed empirically, not just re-derived on paper.
`setup_modelview_matrix()`'s `FSV_DISCV` case never reads
`camera->phi`/`theta` at all (unlike MapV/TreeV) — it applies a *fixed*
`Ry(90°)·Rz(90°)` reorientation to a translate-back-by-distance, and the
resulting modelview's third row (`gpu_mat.modelview[i][2]` across all
`i`, i.e. the matrix row that produces view-space Z) works out to
`(0, 0, 1, -distance)`: view-space Z is world Z minus the camera's dolly
distance, independent of world X/Y. In other words DiscV's camera looks
straight down the world Z axis from `distance` units above it — a
literal top-down "pie chart" view, not a level one — and the original
report's "phi=0, a level camera" description was simply wrong (`phi`
isn't consulted by this code path at all; the fact that the *value*
happened to be 0 doesn't mean it did anything). A ground quad sitting a
fixed 6 units below DiscV's own content plane is squarely inside that
downward view, not off to some unused side of it, whenever it falls
inside the near/far clip band: `camera->near_clip = 0.9375·distance`,
`camera->far_clip = 1.0625·distance` (`camera.c:126-127`), so the ground
(view-space depth `distance + 6`) is in range exactly when
`distance + 6 ≤ 1.0625·distance`, i.e. `distance ≥ 96`. The task's own
verification screenshot used `tests/fixture` (two files, `distance ≈
52`) — comfortably under that threshold — which is exactly why it never
caught this. Re-run on this repo's own `src/` (≈1MB, `distance ≈
1750`+, far over the threshold): confirmed, a full-frame green wall
filling the entire view behind the disc, sky nowhere visible. **Fix**:
`draw_landscape()` now switches on `globals.fsv_mode` and only draws the
ground for `FSV_MAPV`/`FSV_TREEV`; `FSV_DISCV` (and `FSV_SPLASH`/
`FSV_NONE`) get the sky only. The switch lists every `FsvMode` value
explicitly and ends in `SWITCH_FAIL` (no silent `default:`), so `FSV_FSN`
landing in this enum (Task B1) forces a deliberate choice here instead of
inheriting one. Re-verified: the same `--discv --screenshot` on `src/`
now shows the sky gradient with no ground wall.

**Important — the sky's depth-write value could occlude real geometry.**
The original `SKY_NDC_DEPTH = 0.999` trick assumed every real draw's
depth would be nearer, which assumed *linear* depth. It isn't:
`glm_frustum_rh_zo`'s zero-to-one depth is a `1/z`-shaped curve, and
MapV/TreeV's near:far ratio is 128:1 (`camera.h`'s
`NEAR_TO_DISTANCE_RATIO * FAR_TO_NEAR_RATIO` = `0.5 * 128`). Solving
`d(z) = far·(z-near) / (z·(far-near)) = 0.999` for `z` at that ratio
gives `z ≈ 0.887·far` — meaning any real geometry in roughly the outer
11% of the frustum's world-space depth range (closer to camera than the
far clip plane, still legitimately visible) would have its own NDC depth
*greater* than 0.999 and lose `FSV_DEPTH_LESS` to the sky's already-
written value, vanishing behind a background that was supposed to be
infinitely far away. **Fix**: new `gpu.h` enum value
`FSV_DEPTH_ALWAYS_NOWRITE` — `pipeline_for()` builds this pipeline
variant with `enable_depth_test = false` (not merely
`SDL_GPU_COMPAREOP_ALWAYS` with the test still enabled: SDL_GPU mirrors
Vulkan/Metal's rule that a depth *write* only takes effect while the
test itself is enabled, so disabling the test is what actually
guarantees the write never happens, on every backend, rather than
leaning on that rule implicitly). The sky's `SKY_NDC_DEPTH` constant is
now genuinely arbitrary (`0.5`, kept only inside the valid clip range so
the quad isn't near/far-clipped) since depth no longer affects it at
all. `NUM_DEPTH_TESTS` (`src/sdl/gpu.cpp`) went from 3 to 4 for the new
pipeline dimension; `src/ogl-gpu-compat.c`'s `gpu_set_depth_test()`
gained a matching (currently unreachable — GTK's `gpu_set_landscape()`
is still a no-op) `GL_ALWAYS` case so a future GTK landscape
implementation doesn't silently inherit the old `GL_LESS` default.
Re-verified: "slate" is still byte-for-byte identical to the pre-task
screenshot after this pipeline change (re-ran the same stash/rebuild/
diff as the original verification); picking the sky/ground still
resolves to id 0 on all three modes (`--discv`/`--mapv`/`--treev` on
`src/`, sky/ground pixel → `0`, a real node → its real id, same as
before this fix round).

**Minor.** The paragraphs above and the original task report described
DiscV as "phi=0, a level camera" — corrected throughout to "DiscV's
camera never reads `phi`/`theta`; it looks straight down the world Z
axis at a fixed distance", which is what the from-source trace and the
matrix-row derivation above actually show.


### Task B1 verification (FSV_FSN mode — pedestal/wire landscape)

**Files:** `src/geometry-fsn.h` (new — `FsnPedestal`, `FsnWire`,
`FSN_GEOM_PARAMS`, the whole FSN API); `src/geometry-fsn.c` (new —
layout, *gpu-free*, part of `libfsvcore`); `src/geometry-fsn-draw.c`
(new — draw pass, part of each frontend); `src/fsn-style.h` (layout,
wire and text constants); `src/common.h` (`FSV_FSN` enum member);
`src/geometry.c`/`.h` (dispatch arms; `node_set_color()` exported as
`geometry_node_set_color()`); `src/camera.c` (placeholder FSN camera);
`src/colexp.c` (`FSN_COLEXP_TIME`); `src/ogl.c` + `src/sdl/gpu.cpp`
(modelview arm, ground plane); `src/sdl/main.cpp` (`--fsn`);
`src/sdl/ui_main.cpp` (Vis → FSN); `src/meson.build`,
`src/sdl/meson.build`, `tests/meson.build`; `tests/test_fsn_layout.c`
(new).

**The layout, and why it is a separate translation unit from its own
draw pass.** `fsn_geometry_init()` is pure math over the scanned tree: it
writes an `FsnPedestal` into every node's existing `geomparams` scratch
(five doubles — exactly the struct's size, same trick MapV plays) and
three more per directory into `geomparams2` (subtree span, grid
columns/rows). It calls nothing from `gpu.h`. That is what puts
`geometry-fsn.c` in `libfsvcore`, next to `camera.c` — which *reads* the
layout to frame the landscape — and what lets `tests/test_fsn_layout.c`
link exactly like `test_scanfs`, with core objects plus
`tools/fsv-headless-stubs.c` and not one renderer stub. The invariant is
enforced by the linker rather than by a comment: the day the layout pass
grows a `gpu.h` call, the test stops linking. The drawing half lives in
`geometry-fsn-draw.c`, which is frontend-side like `geometry.c` and
appears in both frontends' source lists.

An earlier arrangement had both halves in one file compiled directly
into the test, with a block of `gpu_*`/`text_*` no-ops in the test to
satisfy the linker. It was replaced when `fsv-scan` (which links
`libfsvcore`, and so now links `camera.c`'s new calls into the FSN
layout) failed to link: stubbing `fsn_layout_*` in
`fsv-headless-stubs.c` would have collided with the real definitions in
the test's own copy. The split removes the problem instead of working
around it.

**Coordinates.** `FsnPedestal`'s field names come from the task brief and
follow the graphics convention (`x`/`z` on the ground, height separate).
fsv's world does not: it is z-up, exactly as MapV and TreeV use it. So
`FsnPedestal::z` is the ground *depth* axis and maps to **world y**, and
`::h` is world z. `FsnWire`, which feeds `gpu_draw()` directly, is in
world coordinates. Both are spelled out at the top of `geometry-fsn.h`;
this is the one place in the file pair where the two conventions meet.

**Layout algorithm.** Two passes, because a directory's ground width
depends on its whole subtree and so nothing can be positioned until
everything is measured:

1. *Measure* (bottom-up): file count → a squarish row-major grid of
   `FSN_BOX_EDGE` boxes → the pedestal's footprint (grid plus
   `FSN_PEDESTAL_MARGIN`, floored at `FSN_PEDESTAL_MIN_EDGE`); subtree
   bytes → the pedestal's height; and the subtree's total ground span
   (its own width, or the sum of its children's spans plus
   `FSN_SIBLING_GAP`, whichever is larger).
2. *Place* (top-down): root at the ground origin; files gridded on the
   pedestal top; subdirectories fanned out across the parent's span, one
   `FSN_GENERATION_GAP` further along the depth axis. Because each child
   gets a disjoint slice of ground width, sibling subtrees cannot overlap
   at any depth.

Heights (pedestals and file boxes alike) are `MIN + SCALE * log2(1 +
bytes/1024)`, clamped. Logarithmic rather than linear or sqrt: a real
source tree spans five or six orders of magnitude of subtree size, and
anything gentler leaves the root towering over everything else in the
same frame — the reference screenshot's pedestals are all within a small
factor of each other. The clamp bounds the pathological multi-TB case
outright. All constants are in `src/fsn-style.h` with their rationale.

**Deployment.** Identical contract to MapV: a collapsed directory draws
its pedestal and its own file boxes but neither its children nor their
wires; a directory caught mid-morph draws its children under a z-only
scale, so a subtree grows up out of the ground plane instead of popping
in. Positions never move (the layout is deployment-independent), so only
heights animate. The wire to a child is drawn in the *parent's* unscaled
frame with the child end's height scaled by hand — putting it inside the
scale would drag the parent end down with it.

**Select-pass discipline**, per kind of geometry:

- pedestals and file boxes go through `geometry_node_set_color()`, so
  they paint node ids and are pickable — they are the only FSN geometry
  that *is* a node;
- wires paint **black** (id 0, "nothing there") rather than skipping the
  draw, matching `geometry.c`'s TreeV connectors: a wire that occludes
  something on screen has to occlude it in the pick pass too;
- name labels and the ground path text are absent from the select pass by
  construction — `gpu_pick()` calls `geometry_draw(FALSE)`, and the whole
  text block is gated on `high_detail`.

`geometry.c`'s `node_set_color()` became the exported
`geometry_node_set_color()` for this: one id encoding and one highlight
boost shared by both files, rather than a second copy that could drift
(and `highlight_node_id` stays private to `geometry.c`).

**Camera: a documented placeholder, replaced by Task B2.** FSN reuses
MapV's camera *storage* — the Cartesian XYZ target in `MapVCamera` — so
`camera_pan_finish()`, `camera_pan_break()` and the bird's-eye restore
share MapV's arms outright, and both frontends' `setup_modelview_matrix()`
fall through to the `FSV_MAPV` case. It deliberately does **not** reuse
MapV's camera *math*: `mapv_look_at()`, `mapv_camera_theta()/_phi()`,
`mapv_get_scrollbar_state()` and the bird's-eye distance are all written
in `MAPV_GEOM_PARAMS`, which in FSN mode holds an `FsnPedestal` (the two
modes share `NodeDesc::geomparams`), so delegating would feed the camera
another mode's numbers reinterpreted as its own. `camera_init()`,
`camera_look_at_full()` and `camera_birdseye_view()` therefore get thin
`fsn_*` equivalents shaped like their MapV counterparts but reading the
FSN layout; the scrollbars get the *null* state for the same reason, and
`ui_rail.cpp` keeps Tilt/Height disabled in FSN mode to match.

**Known limitations, all Task B2's subject.** The initial view is
`camera_look_at(root)`, which frames the root pedestal plus one
generation gap. On a wide tree (this repo's own root, ~10 subdirectories)
the child fan is far wider than that frame, so most children sit
off-screen with their wires running out of both edges. That is the
layout being correct and the camera being a placeholder, not a layout
bug — the same tree viewed from `src/` reads exactly like the reference
screenshot. There is also no node cursor and no spotlight in FSN yet, and
`geometry_camera_pan_finished()`'s FSN arm is deliberately empty for that
reason.

**Deviations from the reference screenshot**, deliberate and scoped out
of B1:

- No subdirectory "tower" standing on the parent's own pedestal. Every
  subdirectory is a ground-level pedestal of its own reached by a wire,
  which is what the brief's layout spec asks for; the reference shows
  both idioms.
- Directory name labels are drawn flat on the clear margin band at the
  near edge of the pedestal top (MapV's label idiom), not upright on the
  pedestal's front face. Upright text needs TreeV's rotated-label matrix
  work for a cosmetic gain.
- The ground path text is sized *relative to the root pedestal's width*
  rather than in absolute units, because the camera frames the whole
  landscape: a fixed cap height reads as gigantic on a small tree and as
  a smudge on a large one.

**GTK arm.** Builds and passes tests, but has no Vis → FSN menu entry:
that menu is a GtkBuilder resource (`src/fsv-gresource.xml`) driving
`callbacks.c`'s `on_vis_*_activate()`, and adding an entry there is out
of this task's scope. `FSV_FSN` is reachable in the GTK build only
programmatically. The GTK frontend also keeps its flat clear (its
`gpu_set_landscape()` is still a no-op, Task A1), so FSN there would draw
pedestals with no sky or ground.

**Verification.**

- `meson test` 4/4 on both arms (macOS/SDL and the Debian bookworm
  container's `-Dfrontend=gtk`), including the new `fsn_layout` test. The
  test was confirmed to actually bite by perturbing `FSN_GENERATION_GAP`
  to a negative value (fails) and restoring it (passes).
- Headed, `fsv src --fsn --screenshot`: gradient sky over green ground,
  the `src` pedestal in the foreground carrying a grid of age-colored
  file boxes, two child pedestals (`sdl`, `xmaps`) one generation back,
  white wires from the root's far edge to each child's near edge, the
  `src` name label on the pedestal's near margin, and the absolute path
  written large on the ground in front.
- Picking, via a temporary (reverted) `gpu_pick()` probe in the
  `--screenshot` path: a file box → its file node (`camera.c`,
  `ui_main.cpp`), a pedestal face or a gap between boxes → the directory
  node (`src`, `xmaps`), sky → id 0, ground → id 0, a wire pixel → id 0.
- Expand/collapse, via a temporary (reverted) `colexp()` probe:
  collapsing the root recursively removes both child pedestals and both
  wires, leaving the root pedestal and its file boxes. `colexp` is
  mode-agnostic, so the double-click and context-menu paths that drive it
  need no FSN-specific code.
- No regression in the other modes: `--discv`/`--mapv`/`--treev`
  screenshots still render as before the enum insertion.

**Switch audit.** Every `switch` on `FsvMode` in the tree, and what it
got:

| File | Switch | Treatment |
|---|---|---|
| `src/geometry.c` | `geometry_init()` | arm → `fsn_geometry_init(root_dnode)` (from the root *directory*, not the metanode) |
| `src/geometry.c` | `geometry_draw()` | arm → `fsn_geometry_draw()` |
| `src/geometry.c` | `geometry_camera_pan_finished()` | arm, empty + comment (FSN draws no node cursor, so there is no resting position to record) |
| `src/geometry.c` | `geometry_should_highlight()` | arm → `TRUE` (every directory has a pedestal on screen whether expanded or not, unlike MapV) |
| `src/geometry.c` | `draw_node()` | arm, empty + comment — the function is dead code (`__attribute__((unused))`), like its `FSV_DISCV` arm |
| `src/camera.c` | `camera_init()` | arm — frames the landscape from behind the root pedestal off `fsn_layout_extents()` |
| `src/camera.c` | `camera_scrollbar_moved()` | arm → `fsn_scrollbar_move()` (pan only, no coupled yaw/pitch) |
| `src/camera.c` | `camera_update_scrollbars()` | arm → `null_get_scrollbar_state()` (no FSN scroll model yet) |
| `src/camera.c` | `camera_pan_finish()` | deliberate fall-through into `FSV_MAPV` (shared Cartesian target storage) |
| `src/camera.c` | `camera_pan_break()` | deliberate fall-through into `FSV_MAPV` (same) |
| `src/camera.c` | `camera_look_at_full()` | arm → `fsn_look_at()` |
| `src/camera.c` | `camera_birdseye_view()` pan time | arm → `FSN_CAMERA_MAX_PAN_TIME` |
| `src/camera.c` | `camera_birdseye_view()` going-up | arm — distance from `fsn_layout_extents()`, not `MAPV_NODE_WIDTH()` |
| `src/camera.c` | `camera_birdseye_view()` coming-down | deliberate fall-through into `FSV_MAPV` (same storage) |
| `src/colexp.c` | collapse/expand time | arm → `FSN_COLEXP_TIME` |
| `src/ogl.c` | `setup_modelview_matrix()` | deliberate fall-through into `FSV_MAPV` (identical transform) |
| `src/sdl/gpu.cpp` | `setup_modelview_matrix()` | deliberate fall-through into `FSV_MAPV` (same) |
| `src/sdl/gpu.cpp` | `draw_landscape()` ground gate | arm → `draw_ground = true` (FSN is the mode the landscape exists for) |
| `src/fsv.c` | `fsv_set_mode()` | unchanged — switches on the *previous* mode with a `default:` arm, which is the correct behavior for FSN (remember it as the next startup mode) |
| `src/sdl/ui_dialogs.cpp` | symlink-target eligibility | unchanged — an explicit `== FSV_TREEV` guard for unbuilt TreeV geometry; FSN lays out the whole tree, so it correctly takes the general path |
| `src/sdl/ui_rail.cpp` | Tilt/Height enable | unchanged — `mode == FSV_MAPV \|\| mode == FSV_TREEV`, deliberately excluding FSN, matching its null scrollbar state |
| `src/sdl/main.cpp` | `initial_camera_pan()` | unchanged — an `== FSV_TREEV` guard for the L-shaped pan; FSN takes the ordinary pan |
| `src/window.c:207` (GTK) | **not a switch at all** — `gui_radio_menu_begin(fsv_mode - 1)` depends on the enum's *ordinal* | checked, left alone. This is the class of site a `switch`/`SWITCH_FAIL` grep cannot see, so it is listed here deliberately. It maps the mode onto a radio-item index in the GTK Vis menu, whose items are MapV and TreeV (DiscV is `#if 0`'d out), so `FSV_FSN` would index past the end. It cannot get there: `initial_fsv_mode` is only ever assigned from `--discv`/`--mapv`/`--treev` (`src/fsv.c:232-244`) or from `fsv_set_mode()`'s own bookkeeping, driven by that same two-item menu; the mode is not persisted to `~/.fsvrc` either. FSN has no parser flag, no menu item and no persistence path in the GTK build, so this expression can never be handed it without a code change that would have to add the menu item anyway |

Beyond `switch` statements, the tree was also swept for code that depends
on `FsvMode`'s *numeric order* rather than its members — the failure mode
an enum insertion causes that no switch audit can catch. There is exactly
one such site, `src/window.c:207`, in the table above;
`grep -rn 'fsv_mode *[-+]\|(int)globals.fsv_mode' src/ tools/` finds no
other arithmetic on the enum, and every remaining use is an `==`/`!=`
against a named member.

Non-`FsvMode` switches that `grep SWITCH_FAIL` also matches were checked
and left alone: `src/common.c` (HSV sextant), `src/color.c`,
`src/window.c` (`ColorMode`, `StatusBarID`), `src/animation.c`
(`MorphType`), `src/about.c` (`AboutMesg`), `src/dialog.c`,
`src/colexp.c`'s own `ColExpMesg` switches, and
`src/ogl-gpu-compat.c`/`src/camera.c`'s axis switches.


#### Task B1 fix round (code review)

**Per-frame allocation on the high-detail path (the important one).**
`fsn_draw_path_text()` called `node_absname_display()` every frame, which
walks the node's ancestry, allocates, UTF-8-validates and NFC-composes —
against that function's own documented contract (`src/common.c`: for
callers that fire "on hover/selection changes, not per node per frame").
Now cached: the composed string is kept in the draw file, keyed on the
`GNode *` it was composed from, and recomposed only when
`globals.current_node` moves. It keeps its *own copy* rather than the
returned pointer, because `node_absname_display()` hands back its own
static buffer, which the next caller (status bar, context menu) frees.

The cache is dropped whenever the layout root changes, which covers every
mode switch and rescan. That check lives in `fsn_geometry_draw()`, not in
`fsn_geometry_free()`, because the latter is in the layout half —
`libfsvcore` links it and the draw file is absent there, so a call in
that direction would not link. The key is only ever compared, never
dereferenced.

Measured, not assumed: a temporary counter in `fsn_draw_path_text()` over
a 10-second `--record` run in FSN mode (300 high-detail frames, with the
recording script's two scripted `camera_look_at_full()` cues) logged
**300 draws / 3 cache misses** — one for the initial current node
(`src`), one at frame 141 when the camera looked at `src/sdl`, one at
frame 220 when it looked at `src/sdl/gpu.cpp`. 297 of 300 frames
allocated and composed nothing. The rendered result was checked too: with
the current node pointed at a child, the ground text reads
`…/fsn/fsv/src/xmaps` instead of `…/fsn/fsv/src`, so the invalidation
reaches the screen and not just the counter. Both probes reverted.

**Unchecked nullable deref.** `fsn_draw_path_text()` dereferenced
`fsn_layout_get()`'s documented-nullable return; guarded now, matching
the guard two lines further down.

**Test: vacuous assertions replaced, and the real coverage gap closed.**
Two of the wire assertions could not fail (`wire.x0 == parent->x` by
construction, and `|0| <= positive`). They are replaced by an
`on_footprint_edge()` predicate — the endpoint must lie exactly on the
facing edge's line *and* within that footprint's width — applied by a
recursive walk over every directory in the tree rather than to `dir-a`
alone. The walk returns its visit count, and the caller asserts the exact
number, so it cannot pass vacuously on an empty loop.

The genuinely untested logic was the ground-width slicing:
`FSN_DIR_SPAN`'s "max(own width, children's total)" measuring rule and
the placement cursor's `FSN_SIBLING_GAP` step. The test now asserts that
sibling *subtree* bands — recomputed from the public accessor, not read
back out of the span the layout wrote down — are pairwise disjoint at
every depth. `tests/fixture` grew the shape needed to make both halves
observable: `dir-c` as a second top-level directory, and `dir-d`/`dir-e`
under `dir-a` so that `dir-a`'s subtree is several times wider than its
own pedestal (with only one child, the max() rule has no effect and a
broken version passes). Both halves were then broken on purpose and
confirmed to fail:

| Perturbation | Result |
|---|---|
| `FSN_DIR_SPAN(dnode) = ped->w` (measuring rule ignores children) | FAIL — sibling bands overlap |
| placement cursor advances by 0 instead of `span + FSN_SIBLING_GAP` | FAIL — sibling bands overlap |
| wire anchored at the parent's centre instead of its outward edge | FAIL — `on_footprint_edge` |
| wire endpoint past the child's corner | FAIL — `on_footprint_edge` |

`test_scanfs`'s `>= 6` node-count assertion still passes (it is a floor
by design); its stale comment listing the fixture's exact contents was
rewritten to say so explicitly, so the next fixture addition doesn't look
like it needs an edit there.

**Comments that were wrong or overclaimed.**

- `geometry.c`'s note on the newly-exported `geometry_node_set_color()`
  named `geometry-fsn.c` as its consumer and "size reasons alone" as the
  motive. Both wrong, and dangerously so: the consumer is
  `geometry-fsn-draw.c`, and `geometry-fsn.c` must *never* call it —
  that file is in `libfsvcore` precisely because it touches no renderer
  function, which is the property the layout test enforces at link time.
  Rewritten to name the right file and the renderer-boundary rationale.
- `fsn-style.h` claimed a file box "must never be tall enough to hide the
  pedestal it stands on", which the constants do not deliver
  (`FSN_BOX_H_MAX` 320 ≫ `FSN_PEDESTAL_H_MIN` 24). Weakened to describe
  the tendency, with the reason no clamp was added: box height *is* file
  size, and clamping it against its pedestal would render two equal files
  at different heights depending on which directory they sit in.
- "PURE" on `fsn_geometry_init()` overclaimed. Renderer-free is not
  side-effect-free: like `mapv_init_recursive()`, it reads
  `dirtree_entry_expanded()` and writes each directory's `deployment`.
  Both the declaration and the definition now say what is actually
  guaranteed (no `gpu.h`, enforced by the linker) and what is not.

**Re-verified after the fixes.** Both arms build; `meson test` 4/4 on
each (macOS/SDL and the Debian container's `-Dfrontend=gtk`) with the
enlarged fixture, `scanfs` included; FSN, MapV, TreeV and DiscV
screenshots all still render.

### Task B2 verification (flight navigation — middle-drag velocity model)

**Files:** `src/camera.c`/`.h` (the `camera_flight_*` family, plus
`cancel_pan_for_manual_control()` and flight-end hooks in `camera_init()`,
`camera_look_at_full()` and `camera_birdseye_view()`); `src/fsn-style.h`
(the `FSN_FLIGHT_*` constants); `src/sdl/input.cpp` (the gesture, and this
file's first per-mode branch); `src/sdl/main.cpp` (one `camera_flight_tick()`
call in the main loop). Fold-in from B1's re-review:
`src/geometry-fsn.c`/`.h` + `src/geometry-fsn-draw.c` (layout generation
counter).

**The velocity model.** While the middle button is held, the pointer's
offset *from the press point* — not the delta since the last event — is a
rate:

| pointer offset | maps to | clamped at |
|---|---|---|
| up / down (y) | forward / backward speed along the horizontal view direction | `FSN_FLIGHT_SPEED_MAX` 640 u/s |
| left / right (x) | yaw rate (`camera->theta`) | `FSN_FLIGHT_YAW_MAX` 72 °/s |
| up / down (y) **with Shift** | climb / dive rate (`target.z`) | `FSN_FLIGHT_ALT_MAX` 320 u/s |

Each axis is zero inside a `FSN_FLIGHT_DEAD_ZONE_PX` (6 px) dead zone and
linear in the offset past it (`*_SCALE`), so full deflection is reached
~160 px out — a comfortable drag inside any viewport. Shift *replaces*
forward motion rather than adding to it, so the gesture is a pure
ascent/descent, and it is read live rather than latched at the press, so
it can be tapped mid-flight — on every motion event, and (fix round,
item 3 below) on the Shift press/release itself, so it works with the
pointer parked too.

`camera->phi` is deliberately untouched: fsn's flight was planar, the
pitch is part of the viewpoint rather than part of the flying, and there
is no pitch control (YAGNI — no path-following or wire-riding either).
Altitude has a floor at the ground plane and no ceiling: flying up is
self-limiting (the whole landscape comes into frame), and a ceiling would
have to be recomputed on every rescan.

FSN's camera target is MapV's Cartesian `XYZvec` (B1's storage reuse), so
"position" is `MAPV_CAMERA(camera)->target` and "heading" is
`camera->theta`. Forward on the ground is `-(cos θ, sin θ)`: the camera
sits at `target + distance·(cos θ cos φ, sin θ cos φ, sin φ)` and looks
back down that vector. At B1's initial `FSN_CAMERA_THETA` of 270° that is
`+y`, the direction the tree grows — pushing forward flies *into* the
tree, which is the point of the mode.

**Why a per-tick update and not the morph queue.** A morph has a start
value, an end value and a duration. Flight has a velocity and runs until
the user lets go. Expressing it as a morph would mean either re-arming a
fresh one-frame morph every frame (a `malloc`, a queue insert, a queue
removal and an end callback per frame, to interpolate between two values
one frame apart) or morphing toward a fictitious far-away target and
breaking it on release — which would make the pointer's offset control
*acceleration*, since the morph's own easing would still be shaping the
motion. So the rates live in `camera.c` and `src/sdl/main.cpp`'s loop
calls `camera_flight_tick()` once per iteration, next to
`fsv_animation_tick()`.

The animation subsystem still drives the drawing: every tick that
actually moves the camera calls `redraw()`, which sets
`animation_active`, which is what keeps frames flowing. A tick that finds
all three rates at zero (button held, pointer inside the dead zone)
returns *without* `redraw()` — so holding still costs exactly what not
flying costs. Measured: 122 fps while moving, 60 fps (the loop's idle
`SDL_WaitEventTimeout(…, 16)` path) inside the dead zone and after
release. `run_record_mode()`'s loop deliberately does *not* get the call:
it never runs `input_handle_event()`, so no flight can exist there.

**A latent bug this task had to fix first.** `camera_pan_break()` is not
enough for a caller that simply *stops*: `morph_break()` drops a morph
record without calling its `end_cb`, so the master pan morph's
`pan_end_cb()`/`post_pan_end()` never runs — and that pair is what clears
`camera_currently_moving` and hands the UI back
(`window_set_access(TRUE)`). Every pre-existing caller immediately armed a
replacement pan, master morph included, so none of them noticed. Flight is
the first that doesn't, so it goes through
`cancel_pan_for_manual_control()`, which does that bookkeeping itself.
`geometry_camera_pan_finished()` is deliberately *not* called there: it
records where the node cursor came to rest, and an interrupted pan came to
rest nowhere.

**`window_set_access` stays TRUE during flight**, matching
`camera_dolly()`/`camera_revolve()`, which are the GTK-era precedent for
"the user is steering". That call means "an animation owns the camera,
keep the user off the controls"; flight is the opposite. What flight *does*
mirror from those two is `camera->manual_control = TRUE`, so `colexp.c`
cannot re-aim the camera underneath the user.

**Interaction matrix.**

| Event | Behavior |
|---|---|
| middle press during a pan | `camera_flight_begin()` cancels the pan (see above) and takes over; pose is continuous |
| `camera_look_at_full()` during a flight | calls `camera_flight_end()` first, then pans normally |
| `camera_birdseye_view()` during a flight | same — doubly so going up, since the saved "where the user was" pose would otherwise keep drifting after it was saved |
| `camera_init()` (mode switch, Reset, rescan) | ends the flight |
| Escape during a flight | stops the flight **and nothing else on that press** — the check is before the collapse logic, so the keystroke that pulls you out of a flight does not also close the directory you flew into |
| `input_reset()` (rescan) | ends the flight, alongside the existing capture-grab drop |
| middle drag in DiscV/MapV/TreeV | dolly, unchanged |

**Per-mode gesture dispatch, the pattern.** `viewport.c`'s gestures were
all mode-agnostic — they called `camera.c`'s mode-agnostic entry points
and let *its* per-mode switches decide what they meant. Flight is the
first gesture that is not the same gesture in every mode, so the branch
has to be in `input.cpp`, where the button is known.
`middle_drag_is_flight()` is a `switch` with every enumerator spelled out
and **no `default:`**, deliberately: `-Wswitch` then makes the next member
added to `FsvMode` a compile error in this file rather than a silently
inherited behavior — the same discipline `camera.c`'s `SWITCH_FAIL` arms
enforce at runtime, done at compile time because this one has a
meaningful answer for every mode.

`input.cpp` keeps its own `g_flying` flag rather than asking
`camera_flight_active()`. The two can legitimately disagree for the rest
of a drag: `camera.c` ends the flight on its own when something else
claims the camera, and the button may still be physically held — the flag
is what stops the motion handler resurrecting it on the next event.

**Constants: how they were sized.** Offsets are in **framebuffer pixels**,
not logical points, because `input.cpp` works in pixel space throughout
and its two existing gestures already do. The cost is that a given
physical drag flies twice as fast on a 2× display as on a 1× one — exactly
as it already dollies twice as fast today. Worth revisiting for all three
gestures at once, not for this one alone; recorded here rather than
quietly fixed for flight only.

This repo's own `src/` tree lays out to **1256 × 2224** world units
(`fsn_layout_extents()`, logged during the run below). At the clamped
640 u/s that is 3.5 s along the depth axis and 4.0 s across the diagonal —
inside the brief's 3–5 s target.

**Verification.**

1. **Both arms build.** macOS/SDL (`ninja -C builddir`, clean) and the
   Debian bookworm container's `-Dfrontend=gtk` (53/53 targets, clean).
   `meson test` **4/4 on both** (nvstore, scanfs, color_persistence,
   fsn_layout). `camera.c` is shared, so the GTK build is the real guard
   here — the flight entry points link there too, they just have no
   gesture wired to them.
2. **Headed synthetic flight** (temporary, never-committed `--record`-style
   harness in the main loop, gated on an env var, same throwaway convention
   Tasks A2/A3 used; `SDL_PushEvent()` of real
   `SDL_EVENT_MOUSE_BUTTON_DOWN`/`_MOTION`/`_UP`/`KEY_DOWN` events so they
   go through `ImGui_ImplSDL3_ProcessEvent()` and the real
   `input_handle_event()` path, `io.WantCaptureMouse` gate included —
   logged as 0 at the press). On `fsv src --fsn`, press at (640, 576),
   camera pose logged every frame:

   | phase | pointer offset | measured | expected |
   |---|---|---|---|
   | forward, full | dy = −200 px | **640.26 u/s**, Δz = 0, Δθ = 0 | clamped at `FSN_FLIGHT_SPEED_MAX` = 640 |
   | forward, small | dy = −20 px | **55.97 u/s** | `(20 − 6) × 4` = 56 — linear, unclamped |
   | dead zone | dy = −2 px | **0.00 u/s**, and 60 fps not 122 | inside the 6 px dead zone; no frames requested |
   | yaw right | dx = +200 px | **−72.05 °/s**, ΔXY = 0 | clamped at `FSN_FLIGHT_YAW_MAX`, negative = turning right |
   | Shift + forward | dy = −200 px | **+318.99 u/s in z**, ΔXY = 0 | clamped at `FSN_FLIGHT_ALT_MAX` = 320, no forward motion |
   | release | — | ΔXY = Δz = Δθ = 0 within one tick | motion stops |

   Screenshot pair `b2_flight_start.png` / `b2_flight_end.png` (in the
   task's `screenshots/` directory): the start frame is B1's familiar
   head-on view of the `src` pedestal with `sdl`/`xmaps` one generation
   back; the end frame, after ~4 s of scripted flight, has the viewer past
   and above the two children looking back down two long wires at the root
   pedestal now in the bottom-right corner — the landscape genuinely
   traversed, turned and climbed.
3. **Interaction tests**, same harness:
   - *Press during the intro pan* — before: `flying=0 moving=1 access=0`,
     target y 1097.23; immediately after: `flying=1 moving=0 access=1`,
     target y 1096.35. The pan is broken, the access flag is handed back
     (proving `cancel_pan_for_manual_control()` fires), and the pose is
     continuous — no jump to the pan's end value, i.e. no morph corruption.
   - *Escape during a flight* — `flying=1` → `flying=0`, and
     `dirtree_entry_expanded(current_node)` reads **1 both before and
     after**: the flight stopped and the directory did *not* collapse. The
     same script in `--mapv` (no flight) shows Escape collapsing 1 → 0, so
     the pre-existing behavior is intact where it should be.
   - *`camera_look_at()` during a flight* — `flying=1 moving=0 access=1`
     → in the same call `flying=0 moving=1 access=0`, and the pose then
     morphs normally over the following second.
   - *MapV regression* — the identical script under `--mapv` logs
     `flying=0` throughout, no target motion at all, and `camera->distance`
     changing on each motion event: still a dolly.
4. **Idle CPU unchanged.** Process CPU time over a 10 s idle window in FSN
   mode, measured on this branch and on the immediately preceding commit
   with the same binary path: **0.11 s (new) vs 0.12 s (old)** — identical
   within noise, as expected from `camera_flight_tick()`'s single boolean
   test. Frame pacing during flight is reported in §2 above (122 fps
   moving / 60 fps parked).

**Fold-in from B1's re-review: the path-text cache's pointer identity.**
`geometry-fsn-draw.c` cached the ground label keyed on the `GNode *` it
was composed from, and `fsn_geometry_draw()` dropped the cache when the
layout root changed. Both are pointer comparisons, and GLib's slice
allocator reuses freed node addresses aggressively — so after a Change
Root or a Rescan a stale key can compare equal to a live node from an
entirely different tree, and the label would keep showing the old path.
B1's own comment dismissed this as not worth a generation counter; that
was wrong, because the residual it described (a rescan of the *same*
directory, which composes the same string anyway) is not the only case.

`fsn_geometry_init()` now bumps a counter exposed as
`fsn_layout_generation()`, and the cache key is the `(node, generation)`
pair. The root-change check is kept as well — it drops the allocation
promptly on a mode switch — but it is explicitly no longer the
load-bearing one, and the comment says so.

Verified with a temporary draw/miss counter and a scripted
`app_request_rescan()`: **725 high-detail draws, 2 misses** (one at
startup, one at the rescan). To prove the generation is doing the work
rather than riding on the pointer checks, both the root-change
invalidation *and* the node-pointer half of the key were then
short-circuited to `0` and the run repeated: still exactly **2 misses**,
the second logged with the key still holding the *old* node pointer while
`gen=2 keygen=1`, followed by 482 clean hits. Both probes reverted before
the commit.

#### Task B2 fix round (code review)

Two important findings and four minors. Commits `b3e6cd2` (input.cpp) and
`7bd76ff` (camera.c).

**1. A stale `g_flying` swallowed Escape (important).** `camera.c` ends a
flight *unilaterally* whenever something else claims the camera:
`camera_look_at_full()` (a click on a pedestal, a tree row, Go Back, the
rail's Look At), `camera_birdseye_view()` (the rail's Bird's Eye) and
`camera_init()` (a mode switch, Reset, a rescan). None of those go
through `input.cpp`, so its `g_flying` stayed true with no flight behind
it — and every branch gated on it then misfired. The visible one:
Escape's flight check consumed the keypress on a do-nothing
`stop_flight()` instead of falling through to the collapse logic, so
**Escape appeared dead for as long as the middle button stayed down**.
The middle-drag was left inert after a mode switch for the same reason.

Fixed by reconciling once at the top of `input_handle_event()`
(`reconcile_flight_state()`: `if (g_flying && !camera_flight_active())
g_flying = false;`), which covers every unilateral-end path at once
rather than patching the Escape branch alone. Only that direction needs
reconciling — `camera.c` never *starts* a flight by itself.

**2. Yaw normalization made the next pan whip the long way round
(important).** `morph()` interpolates linearly between two *numbers*, and
theta is an angle. `camera_flight_tick()` normalizes theta into [0, 360]
on every tick, so a viewer who has turned slightly past the wrap point
leaves theta at ~3° — and `fsn_look_at()`'s morph to `FSN_CAMERA_THETA`
(270) then spun **267° the long way**, over a second of gratuitous yaw,
instead of the 93° short arc.

`unwrap_theta_toward()` shifts `camera->theta` by whole turns until it is
within 180° of the target before the morph is armed. Free, because every
other consumer of theta takes its sine or cosine — theta and theta ± 360
are the same heading everywhere; only the morph, which does arithmetic on
the number itself, can tell them apart. Applied to `fsn_look_at()` and to
`camera_birdseye_view()`'s FSN going-up arm, which is the other pan a
flight can hand a wrapped heading to. **Not** applied to
`camera_revolve()`, whose identical normalization gives DiscV/MapV/TreeV
the same long-way-round pan after a manual revolve: pre-existing upstream
behavior in three modes this task is not touching, recorded here rather
than changed under cover of an fsn task. **Closed post-v0.3**: all
birds-eye arms now call `unwrap_theta_toward()` at entry, including the
MapV/TreeV going-down restoration; see meson test `camera_theta`.

**3. Shift was only sampled on motion events (minor).** The rate model
makes holding the pointer still a legitimate way to fly — and no motion
event arrives while it is still, so pressing Shift did nothing until the
user jiggled the mouse. The last offset handed to
`camera_flight_update()` is now cached and re-applied from Shift
`KEY_DOWN`/`KEY_UP`. `SDL_GetModState()` is read at apply time rather
than derived from the event, so releasing one Shift while the other is
held correctly stays "Shift down". The claim in this document's Task B2
note that Shift "is read live … so it can be pressed and released
mid-flight" was true only while the pointer was moving; it is now true
unconditionally.

**4. `camera_flight_end()` depended on `globals.fsv_mode` (minor).** It
called `camera_update_scrollbars(TRUE)`, which switches on the current
mode and ends in `SWITCH_FAIL` for `FSV_NONE` — and two of its callers
run at moments when that variable does not describe the world:
`camera_init()`, which `fsv_set_mode()` calls *after* the new mode's
`geometry_init()` but *before* assigning `globals.fsv_mode`, and
`input_reset()`, which runs around a rescan. It was safe only by an
accident of ordering, which is exactly the kind of thing a later
reordering breaks silently. Removed rather than defended: the call was
redundant in both reachable cases (a flight that moved the camera already
pushed state from its last tick; one that never moved it has nothing to
push), and dropping it makes the function mode-independent by
construction. Nothing else relied on it — the only reader of that state
is the rail's Tilt/Height sliders, which are disabled in FSN mode
anyway. The `camera_init()` comment added in `130dea5`, which reasoned
about the old ordering hazard, is replaced by one that just points at the
new invariant.

**5–6. Comments (minor).** `cancel_pan_for_manual_control()` was explicit
about skipping `geometry_camera_pan_finished()` but silent about
`filelist_show_entry()` — a no-op in the SDL frontend today
(`src/sdl/stubs.c`) but real in the GTK one and a candidate to become
real here, so it now says why: the pan's destination never became the
current node, so nothing should select it. And `camera.c`'s `fsn-style.h`
include comment still claimed the file was included for
`FSN_GENERATION_GAP` alone.

**Fix-round verification.** Same throwaway `SDL_PushEvent()` harness as
the Task B2 note, extended and again deleted before commit. Both arms
rebuilt, `meson test` **4/4 on each**.

- **The Escape repro, both directions.** Script: FSN, middle-press,
  fly, `camera_look_at(root_dnode)` (ends the flight from camera.c's
  side, middle button still down), then Escape.
  `dirtree_entry_expanded(root_dnode)` logged either side of the
  keypress. **With `reconcile_flight_state()` commented out: 1 → 1** —
  the press vanished, exactly as reported. **With it: 1 → 0** — Escape
  collapses as normal.
- **Escape during a *genuine* flight still stops the flight and nothing
  else** (the original B2 behavior, re-checked because the reconcile sits
  in front of it): `flying=1 → flying=0`, `rootexp` unchanged at 1.
- **Short-arc pan.** Flew forward, then yawed left ~90° until theta
  wrapped to **3.673**. `camera_look_at(root_dnode)` logged immediately
  before and after the call: theta **3.673 → 363.673**, i.e. unwrapped in
  place, |363.673 − 270| = **93.7° ≤ 180**. The pan then swept
  monotonically down through 328° toward 270 with no wrap-around spin.
- **Shift while parked.** Middle-press, one motion to full forward
  deflection, then **no further motion event at all** — just
  `SDL_SetModState()` + a synthetic Shift `KEY_DOWN`. Altitude went
  212.34 → 401.62 in 0.60 s = **315 u/s** (`FSN_FLIGHT_ALT_MAX` is 320)
  while x/y froze after one frame of in-flight residual (5.6 units ≈ one
  640 u/s frame, the queued event being drained on the next iteration).
  A synthetic Shift `KEY_UP`, again with no motion event, resumed forward
  travel at **628 u/s** and stopped the climb.
- **Regressions.** Flight-during-an-intro-pan still breaks the pan with a
  continuous pose and `access` back to 1 (target y 1097.03 → 1096.16);
  plain forward flight still clamps at **640 u/s**; MapV middle-drag
  still dollies (`flying=0` throughout, `distance` 1468 → 2042 → 1244,
  `theta` and `target` untouched) and the new Shift key handling is inert
  there; `--screenshot` renders unchanged in both FSN and MapV.

### Task B3 verification (selection spotlight + FSN polish — Milestone B complete)

**Files:** `src/gpu.h` (new `FSV_DEPTH_LESS_NOWRITE`), `src/sdl/gpu.cpp`
(`pipeline_for()`'s new depth/blend branch, `NUM_DEPTH_TESTS` 4→5),
`src/ogl-gpu-compat.c` (matching, currently-unreachable case),
`src/fsn-style.h` (`FSN_LANDSCAPE_CLASSIC`, the `FSN_SPOTLIGHT_*`
constants and the `fsn_spotlight_rings[]` table), `src/geometry-fsn-draw.c`
(`fsn_node_visible()`, `fsn_gldraw_spotlight_ring()`, `fsn_draw_spotlight()`,
wired into `fsn_geometry_draw()`), `src/color.c`/`.h`
(`landscape_explicit()`/`landscape_set_auto()`), `src/sdl/main.cpp`
(`enter_fsn_mode_landscape()`), `src/camera.c` (`camera_birdseye_view()`'s
going-down arm).

**The decal, and why it is stacked rings rather than a true gradient.**
The brief's ideal is a triangle-fan radial gradient via per-vertex alpha.
`FsvVertex` (`src/gpu.h`) has only `pos`/`normal` — no per-vertex color —
so, exactly like Task A1's banded sky, that gradient is not expressible
through `gpu_draw()` as it stands, and adding a new vertex format/pipeline/
shader pair purely for a decorative ground decal would be exactly the
contract growth the brief asks to avoid. Instead `fsn_draw_spotlight()`
draws `FSN_SPOTLIGHT_RING_COUNT` (6) concentric filled ellipses, largest
(faintest) first, each a flat-alpha `FSV_TRIANGLE_FAN`; standard "over"
alpha compositing across the six accumulates a stepped approximation of
the target falloff (composited alpha roughly 0.05/0.12/0.20/0.29/0.41/0.52
outermost-to-innermost, against the brief's ~0.55 center target).

**Where the ellipse sits.** `fsn_layout_get(node)` already carries the
selected node's own `x`/`z` ground position (both directories' pedestals
and files' boxes are fully positioned by `geometry-fsn.c`'s placement
pass) — no new accessor was needed. What differs between the two node
kinds is the *surface* the ellipse lies on: a directory stands on the
true ground (world `z == 0`), but a file stands on its *parent's*
pedestal top (world `z == parent_ped->h`) — read via a second
`fsn_layout_get(node->parent)` call. `FSN_SPOTLIGHT_LIFT` (0.5, its own
constant rather than reusing `FSN_TEXT_LIFT`) keeps the decal off
whichever surface that is. Sizing: `FSN_SPOTLIGHT_FILE_SCALE` (2.0×
the box footprint) for a file, `FSN_SPOTLIGHT_DIR_SCALE` (1.15×) for a
directory — both eyeballed per the brief.

**The one new pipeline variant, and why it needed no new gpu.h entry
point.** `FSV_DEPTH_LESS_NOWRITE` extends `FsvDepthTest` (mirroring
Task A1's `FSV_DEPTH_ALWAYS_NOWRITE` addition): depth test LESS, write
off. Because `gpu_draw()` already picks its pipeline from whatever
`gpu_set_depth_test()` last set, tying alpha blending (the text
pipeline's SRC_ALPHA/ONE_MINUS_SRC_ALPHA factors) to this one enum value
in `pipeline_for()` meant the spotlight needed nothing beyond the
existing `gpu_set_color()`/`gpu_set_lighting()`/`gpu_set_depth_test()`/
`gpu_draw()` contract — no `gpu_spotlight()`, no new topology flag. The
brief flagged both options; this was the smaller one. `ogl-gpu-compat.c`
gained a documented, currently-unreachable `GL_LESS` case for the same
reason `FSV_DEPTH_ALWAYS_NOWRITE` did: FSN has no GTK menu entry, so
nothing there ever passes this value, but a future GTK landscape/decal
should not silently inherit the wrong depth func from the `default:`
case.

**A real visibility bug caught by testing the "collapse the parent" case
literally, not just filing it as done.** The first `fsn_node_visible()`
walked from `node->parent` up to the root, hiding the spotlight whenever
*any* ancestor was collapsed — including the node's own immediate
parent. That is backwards for a **file**: `fsn_draw_recursive()` draws a
directory's own box *and* own files unconditionally, before it ever
checks its own `collapsed` flag; that flag only gates the loop that
recurses into the directory's *child directories*. So collapsing a
directory hides its subdirectories (transitively) but never its own
files, and never itself. Confirmed both directions with the real
draw path (see below): selecting a file directly under `src/`
(`color.c`) and collapsing `src` itself left the spotlight showing
(`src` is `color.c`'s own directory, and `src`'s own files always draw
once `src` itself is reached — which it always is, being the root);
selecting a *directory* (`src/sdl`) and collapsing its parent (`src`)
correctly hid the spotlight (and the `sdl` pedestal itself — nothing
recurses into a collapsed directory's children at all). The fixed
`fsn_node_visible()` starts its ancestor walk one generation higher for
a file than for a directory, with the difference spelled out in its own
comment.

**Auto-landscape.** `color.c` gained `landscape_explicit()` (persisted
nvstore boolean, `landscape_set()`'s one and only caller being the
Display menu) and `landscape_set_auto()` (same apply-and-persist body,
factored into a shared `landscape_apply()`, but leaves the explicit flag
untouched). `src/sdl/main.cpp`'s `enter_fsn_mode_landscape()` runs on
every entry into `FSV_FSN` (both `enter_mode()`'s and `run_mode_entry()`'s
`globals.fsv_mode = mode;` line) and auto-selects `FSN_LANDSCAPE_CLASSIC`
unless the flag is already set. Leaving FSN restores nothing — no
"landscape before FSN" is saved anywhere, so whatever FSN leaves selected
(whether auto or explicit) simply persists afterward; this asymmetry is
intentional (YAGNI) and documented at the call site, not an oversight.

**Birdseye fold-in.** `camera_birdseye_view()`'s going-down (`else`) arm
restores `camera->theta` to `pre_cam->theta` with a plain `morph()`,
exactly like the going-up arm's `new_cam->theta` target did before
Task B2's fix round — and `morph()` interpolates the raw number, not the
angle, so a flight-wrapped `camera->theta` (e.g. left at 3.673 after
several full turns) would restore via the long way around. Fixed with
the same `unwrap_theta_toward( pre_cam->theta )` call the going-up arm
already makes, gated to `FSV_FSN` for the same reason that arm's call
is: DiscV/MapV/TreeV never wrap theta the way a flight does, and their
own pre-existing "long way round" behavior after a manual revolve
(recorded in the Task B2 fix-round note) is out of scope here.

**Follow-up (post-v0.3, user QA on the framing fix):** the pool alone
proved nearly invisible in real use — white at ~0.55 composited alpha
on a light-grey pedestal top, mostly self-occluded inside a packed box
row. `fsn_draw_spotlight()` now also draws US5861885's actual beam: a
truncated translucent cone (`FSN_SPOTLIGHT_CONE_*`, fsn-style.h) from a
node-height-scaled apex down to the pool ellipse, silhouetting against
sky and pedestal faces instead of the surface it sits on. Same guards
by construction (drawn by the same function); same deployment-caveat as
the pool (see *Concerns / disclosed gaps* below). Tuned via `--screenshot`
captures, throwaway `FSV_TEST_SELECT` harness, removed before commit.

**Verification.**

1. **Both arms build clean, `meson test` 4/4 on each.** SDL/macOS
   (`ninja -C builddir-sdl`, incremental and from-scratch). GTK: fresh
   `debian:bookworm` container, this repo's CI apt list, `meson setup
   -Dfrontend=gtk && ninja` (53/53 targets) `&& meson test` → 4/4, run
   twice — once mid-task to confirm the base spotlight/pipeline work
   didn't regress the GTK build, once at the end after the
   `fsn_node_visible()` fix and after all throwaway debug code below was
   removed.
2. **Headed FSN on this repo's own `src/`.** A throwaway, env-var-gated
   block in `src/sdl/main.cpp`'s `--screenshot` path (same convention as
   Tasks A2/A3/B2: `FSV_TEST_SELECT`/`FSV_TEST_COLLAPSE`/`FSV_TEST_PICK`/
   `FSV_TEST_BIRDSEYE`, resolving paths via `common.c`'s `node_named()`
   and driving `colexp()`/`camera_look_at()`/`camera_birdseye_view()`
   directly — never committed, removed before this task's commits) plus
   a temporary `fprintf` in `fsn_draw_spotlight()`:
   - **Select a file** (`src/color.c`, a root-level file): flying the
     camera to frame `color.c`'s own tiny 64-unit footprint
     (`fsn_look_at()`'s `SQRT_2 * max(w,d)` diameter rule) puts the
     camera nose-first against the packed box row with no ground in
     frame at all — an honest finding about that zoom rule on a densely
     packed real source tree, not a spotlight bug (recorded as a
     concern below). Framing the camera on the *parent* directory
     instead (while leaving the actual selection on the file) gives a
     legible shot: a soft, pale, elliptical glow pools at the base of
     `color.c`'s box on the grey pedestal surface, brightest near the
     box and fading outward over the six rings — cropped/zoomed
     screenshots read exactly like the reference's "soft white
     elliptical light pool," just smaller in an unzoomed frame because
     this repo's own files are modest in size. Confirmed the glow moves
     when a different file is selected (checked against the logged
     `cx`/`cz`/`base_z` from `fsn_layout_get()`).
   - **Select a directory** (`src/sdl`): the ellipse is only 1.15× the
     pedestal's own footprint, and the pedestal is a fully opaque block
     standing on exactly that footprint, so almost the entire decal is
     self-occluded by the pedestal it surrounds — only a thin, correctly
     visible rim peeks out at the pedestal's outward corners/edges in
     the oblique default view. Subtler than the file case by
     construction (a directory's own footprint dominates its ellipse far
     more than a file's does), not a bug.
   - **Click on nothing (sky/ground):** traced through
     `src/sdl/input.cpp` rather than screenshotted — `g_indicated_node`
     is set to `node_at_cursor()`'s result on press, but
     `camera_look_at()` (the only writer of `globals.current_node`) on
     button-up is gated on `g_indicated_node != NULL`; a click resolving
     to id 0 leaves it NULL, so `camera_look_at()` never runs and
     `globals.current_node` — and therefore the spotlight — is
     unchanged. There is no deselect gesture in this port today.
   - **Collapse the selected node's parent:** see the visibility-bug
     writeup above — both the directory case (spotlight and pedestal
     both vanish) and the file-under-root case (spotlight correctly
     survives collapsing root, since root's own files always draw) were
     exercised via `FSV_TEST_COLLAPSE` and confirmed by screenshot.
3. **Pick-through.** `FSV_TEST_PICK="650,420"` (a pixel inside the
   visible glow from the file screenshot above) called `gpu_pick()`
   directly: it returned a small, ordinary node id (13 — the pedestal
   under the decal) with **no** `fsn_draw_spotlight()` debug line
   printed during that call's internal offscreen render (one debug line
   total per frame, from the *normal* pass that ran afterward) —
   confirming the decal's own `gpu_render_mode() != FSV_RENDER_NORMAL`
   self-check keeps it out of the select pass entirely, exactly like
   `fsn_gldraw_wire()`'s pattern elsewhere in the same file.
4. **Auto-landscape.** A standalone harness (same isolation technique as
   `tests/test_color_persistence.c`: `$HOME` redirected before anything
   touches `~/.fsvrc`, linked straight against `libfsvcore`'s real
   `color.c`, not added to `meson.build`) reproduced
   `enter_fsn_mode_landscape()`'s one-line body (that function itself is
   C++/SDL-only and wasn't linked) and ran the brief's exact scenario:
   fresh config → `landscape_get()==0` (classic) after the simulated FSN
   entry, `landscape_explicit()==FALSE`; `landscape_set(2)` (simulated
   menu pick of "slate") → `landscape_explicit()==TRUE`; a second
   simulated FSN entry → `landscape_get()` stays `2` (slate sticks); a
   fresh `landscape_init()` (simulated relaunch) → both the preset and
   the explicit flag reload correctly from `~/.fsvrc`. All four
   assertions passed.
5. **Birdseye fold-in.** `FSV_TEST_BIRDSEYE=1` set `camera->theta = 3.673`
   directly (via `camera.h`'s exported `extern Camera *camera`), then
   called `camera_birdseye_view(true)` (drained to settle at
   `theta==270`, the going-up arm's already-proven unwrap) and
   `camera_birdseye_view(false)`: **immediately** after the call (before
   the restore morph has run at all) `camera->theta` read **−90.0** —
   `unwrap_theta_toward(3.673)` applied to 270 (`|270 − 3.673| = 266.3 >
   180`, so shifted by one turn to −90, `|−90 − 3.673| = 93.7 ≤ 180`,
   stop) — then settled at **3.673** once the morph completed, i.e. a
   monotonic 93.7° arc, not the 266° long way. Matches Task B2's own
   verification style for the going-up arm and `fsn_look_at()` exactly.
6. **All throwaway harness code removed before the commits below** —
   confirmed by re-diffing `src/sdl/main.cpp` and
   `src/geometry-fsn-draw.c` against the previous commit and rebuilding
   both arms clean one final time.

### Task C1 verification (overview window — picture-in-picture mini-map)

Upstream fsn's "overview" window (reference screenshot
`3060c037-069f-4715-a01e-c30e53e505a2.jpg`, top-right): a small live map
of the whole landscape seen from straight above, with a marker at the
camera's own position. FSN mode only, toggled from **View → Overview**
(greyed out in the other modes), default on.

**Where the pieces live.** `src/geometry-fsn.c` gains two pure-math
accessors (`fsn_layout_bounds()`, the landscape's ground box in absolute
world coordinates — the extents alone cannot frame a landscape that does
not straddle the origin — and `fsn_layout_nearest()`, point → pedestal,
descending exactly as far as the draw pass does). `src/sdl/gpu.cpp` gains
`gpu_overview_render()`, the **fourth** offscreen render path in that
file after `gpu_pick()`, `--screenshot` and `--record`.
`src/sdl/ui_overview.cpp` owns the ImGui window, the click mapping and
the re-render policy.

**Texture binding.** ImGui 1.92.9b's SDL_GPU backend takes a raw
`SDL_GPUTexture*` as its `ImTextureID` and supplies its own sampler
(`imgui_impl_sdlgpu3.cpp`: `texture_sampler_binding.texture =
(SDL_GPUTexture*)(intptr_t)pcmd->GetTexID()`, sampler =
`bd->CurrentSampler`). Verified by reading the vendored backend rather
than assumed — **before 2025/08/08 the same backend wanted a pointer to
an `SDL_GPUTextureSamplerBinding`**, and handing one API the other's
value crashes. `ImGui::Image()` takes an `ImTextureRef`, which has an
implicit constructor from `ImTextureID`, so the cast is the whole story;
the texture carries `SDL_GPU_TEXTUREUSAGE_SAMPLER` alongside
`COLOR_TARGET`.

**Resolution: fixed, 512x320** (`FSN_OVERVIEW_WIDTH/HEIGHT`,
`src/fsn-style.h`), letterboxed into whatever size the user drags the
window to. Window-sized would mean destroying and recreating both the
color texture *and* its depth buffer on every drag frame; 512x320 is
already more pixels than the 320x200 default window shows, and 16:10
makes that default close to pixel-for-pixel.

**Not routed through `g_capture_texture`.** Unlike the other three
offscreen paths, the overview has its own texture *and* its own depth
buffer, both cached until `gpu_shutdown()`, and `gpu_scene_begin()`/
`gpu_scene_end()` branch on `g_overview_frame` **before** consulting the
capture state. Two consequences, both deliberate: an overview render
nested inside a `--record` frame still picks the R8G8B8A8 pipelines that
match its own texture (the record texture is in the swapchain's format,
target index 0), and `ensure_depth_texture()`'s single window-sized depth
texture is never made to flip-flop between two sizes twice a frame.

**Re-render policy: only when something it shows has moved.**
`ui_overview_render()` compares a key of {mode, `fsn_layout_generation()`,
the camera's six pose numbers, a walk-summed total of every drawn
directory's `deployment`} against the last render's. The deployment sum
is the awkward member and is there on purpose: expanding or collapsing a
directory from the context menu changes the map with no camera movement
at all, and `src/animation.h` has no "a morph is running" predicate to
ask instead. It costs one float-summing tree walk on frames that are
being drawn anyway, next to the two full walks `geometry_draw()` already
performs.

**Scene only, and thinner than the main view.** No ImGui inside the
texture; no sky (a stack of screen-space quads would simply cover a
top-down map) and no ground quad (the pass's clear color *is* the
ground, pinned to the "night" preset — whose ground is the same green as
"classic"; only its sky, which the overview never shows, differs); no
labels or path text (`geometry_draw(FALSE)`, exactly as `gpu_pick()`);
and no selection spotlight, which is skipped by `geometry-fsn-draw.c`
itself on a new `gpu_overview_pass()` predicate. That predicate is
deliberately **not** a third `FsvRenderMode` value: the overview paints
real colors, so all four existing `gpu_render_mode() == FSV_RENDER_NORMAL`
tests must keep answering "normal" during it. The GTK shim returns 0.

**Marker.** A flat arrowhead at the camera's ground position — derived
by solving `setup_modelview_matrix()`'s own transform for the eye point,
`camera = target + distance * (cos φ cos θ, cos φ sin θ, sin φ)` — 
pointing back along `-(cos θ, sin θ)`, drawn with the depth test off so a
pedestal it stands over cannot hide it. Sized as a fraction of the framed
half-width (constant on screen at any landscape scale) and **clamped into
the framed rectangle**: the frame is the landscape's box, not the box
extended to include the camera, so that flying does not continuously
rescale the map — "stable map, moving marker" — and a camera pulled back
outside the tree pins its marker to the edge instead of vanishing. The
reference's marker is a small black X; this is a yellow arrow, because it
has to read against nodes colored by type/timestamp and because an arrow
also carries the heading (documented as a deliberate departure in
`fsn-style.h`).

**Click-to-look-at**, no drag-navigation (YAGNI, per the plan): click in
the image → item-relative UV → the *same* world rectangle the last render
framed (`gpu_overview_frame_rect()`, not a recomputation) → 
`fsn_layout_nearest()` → `camera_look_at()`. The only subtlety is the
vertical flip: world +y is NDC +y is the texture's *top* row, so `v == 0`
maps to `max_y`.

Verified:

1. **Both arms build**, `meson test` **4/4 on each** — macOS/SDL native
   and a Debian bookworm container's `-Dfrontend=gtk` (53/53 targets).
   `tests/test_fsn_layout.c` grew invariants 7 and 8 for the two new
   accessors, including the not-origin-centered property a framing
   consumer would otherwise get wrong and the collapsed-subtree case.
2. **Headed FSN on `src/`**: the overview shows the three-pedestal
   landscape from above, file-box grids and both wires visible, marker at
   the camera. Screenshots `c1_overview_full.png` (whole window) and the
   pair `c1_overview_marker_start.png` / `c1_overview_marker_turned.png`
   — after a scripted 6-second revolve the marker has walked from the
   bottom edge (pointing +y, into the landscape) round to the left edge,
   **turned 90° to keep pointing back at the tree**: it moves *and*
   rotates. `c1_overview_after_flight.png` shows the same after a
   `camera_look_at()` flight to `src/sdl`.
3. **Click-to-look-at**: a synthetic click at logical (1200, 60), pushed
   as real SDL events through `ImGui_ImplSDL3_ProcessEvent()`, logged
   `overview: click (1734.7, 1693.9) -> xmaps` and the camera target then
   morphed (0,0) → (416.0, 1623.9), i.e. it genuinely flew to that
   pedestal.
4. **Idle**: **0 overview renders over 119 consecutive forced frames**
   with the camera settled (temporary counter, removed before the
   commits). During a flight it re-renders every frame, as intended.
   Hiding the window: **0 renders over 40 frames of live camera motion**.
   In MapV: **0 renders over 40 frames of motion**, and the window is not
   drawn at all.
5. **Pick regression, interleaved in the same frame.** A 7-point
   `gpu_pick()` sweep across the window's center row returned
   `86 99 80 89 88 101 0` with the camera parked (no overview render that
   frame) and **byte-identical ids** while the overview was re-rendering
   on every frame — the two offscreen paths do not disturb each other.
   `--screenshot` still renders in all four modes (FSN/MapV/TreeV/DiscV);
   `--record` wrote 330 FSN frames with the overview composited in.
6. **All throwaway harness code removed before the commits below** —
   `src/sdl/main.cpp` and `src/sdl/ui_overview.cpp` re-diffed against the
   previous commit and both arms rebuilt clean afterwards.

### Concerns / disclosed gaps

- **The overview never appears in `--screenshot` output.** That path
  renders the scene alone with no ImGui pass (by design, since Task 2.2),
  and the overview is an ImGui window. `--record` composites ImGui and
  does show it. Not a defect, but it means the mini-map cannot be
  regression-checked by the cheap headless screenshot the rest of this
  port leans on.
- **The mini-map's framing ignores the camera.** A camera far outside the
  landscape pins its marker to the frame edge rather than zooming the map
  out to include it — the deliberate trade for a map that does not
  rescale under every flight, but it does mean the marker's *distance*
  from the tree is not readable while it is clamped.
- **`fsn_look_at()`'s file-zoom diameter (Task B1, not touched here)
  makes a real click-to-fly on a file in a densely packed directory land
  the camera nose-first against the box row**, as noted above — a real
  UX rough edge surfaced by this task's own verification, not a
  spotlight defect, and out of B3's scope to fix (it is `camera.c`'s
  framing rule, not the decal). **Update (post-v0.3): fixed.** Commit
  `74df5a5` added the fix: `fsn_look_at()` now frames a file by its parent
  directory's footprint (wire arm included), target unchanged on the file
  — the parent-framing shot this very verification recorded as legible.
  Regression-locked by `tests/test_fsn_framing.c` (meson test
  `fsn_framing`) — verified numerically (distance parity + target-on-file);
  the headed 30-second QA on a dense tree is still pending.
- **The spotlight is not deployment-aware.** If the selected node's
  ancestor chain is mid-collapse/expand morph (`deployment` strictly
  between 0 and 1), `fsn_draw_spotlight()` draws at the node's static
  layout height regardless — unlike `fsn_gldraw_wire()`, which scales its
  child endpoint by `deployment` by hand. A visible glitch only during
  the brief animation window, and only for a node whose ancestor is
  *also* the one currently being expanded/collapsed; accepted as YAGNI
  for a decorative decal rather than threading deployment through
  `fsn_draw_spotlight()`. The light cone (follow-up above) amplifies the
  same glitch rather than adding a new one: its apex rides `ped->h` off
  the same static layout height the pool uses, so during the morph
  window it is a 384–1536-unit beam anchored mid-air off a height the
  node doesn't actually have yet, proportionally taller and more visible
  than the pool's own mis-anchoring.
- **The selected node's own color visibly desaturates under the beam** —
  roughly 43% saturation loss measured against the unselected color. This
  is the translucent-decal trade-off inherent to painting a white,
  alpha-blended cone directly over the node's own box faces (same
  mechanism as the pool); `FSN_SPOTLIGHT_CONE_ALPHA` and the ring alphas
  are re-tunable if a future pass wants less wash-out, but the whole
  design leans on "over" compositing (see the ring-table comment above),
  so some desaturation under the decal is inherent to the approach, not a
  bug to fix in isolation.
- **The beam disappears entirely when the camera is inside the cone.**
  The scene pipeline back-face culls (see `fsn_gldraw_spotlight_cone()`'s
  own comment), so a camera whose position is within the cone's radius
  sees only back faces and the beam vanishes — reachable in practice via
  warp-lite (Task C4)'s `camera_warp_to()`, which can land the camera
  inside a directory's own cone. Benign: recorded via a warp-lite pose in
  this task's own `--screenshot` captures (nothing else in the scene is
  affected), not a rendering defect. A fade-on-entry guard — skip or fade
  the cone once the camera's horizontal distance to its axis drops inside
  the cone's radius at camera height — is ticketed in `TODO.md` rather
  than fixed here, since it would need the camera's live position
  threaded into a function that today only reads layout state.
- **"Night" landscape is now calibrated** (Task A1's disclosed gap, closed
  post-v0.3; the sky colors were determined by screenshot iteration against
  established constraints, validated at default and tilted framings; ground
  is unchanged from "classic" and load-bearing for the overview mini-map pin).

### Task C2 verification (Marks panel — named node bookmarks)

Upstream fsn's left-rail "Marks" list (task-C2-brief.md's reference
screenshot): named bookmarks of nodes in the landscape, with "go to" and
"delete" per row and a "Mark here" button that bookmarks
`globals.current_node`. **A slight extension of the original**: shown in
every mode, not FSN-only — a mark is just a node bookmark, and there's
nothing FSN-specific about wanting to jump back to a node from MapV or
TreeV either. It joins `src/sdl/ui_rail.cpp`'s camera rail below its
Tilt/Height sliders.

**Files:** `src/common.c`/`.h` (`node_from_absname()` — a purpose-named
entry point wrapping the already-complete `node_named()`); `src/sdl/
ui_rail.cpp`/`.h` (`ui_marks_init()`, the whole Marks section);
`src/sdl/main.cpp` (`ui_marks_init()` call at startup); `tests/
test_fsn_layout.c` (invariant 9).

**Storage: paths, not pointers — same UAF discipline as Task B1.** Each
mark is `{name, path}`, where `path` is `node_absname()`'s raw-byte
absolute path. A rescan or Change Root frees and rebuilds the whole
fstree, so a `GNode *` captured before that would dangle; a path string
survives it and is resolved back to a live node only at draw time (to
grey out a missing row) and at "go to" time, via the new
`node_from_absname()`. This is cheap enough to redo every frame for a
handful of marks and needs no dedicated "invalidate on rescan" hook.

**`node_from_absname()` turned out to already exist, under a different
name.** The brief asked to "compose a resolve helper... `node_from_absname(
)` — verify against `node_absname()`'s format so round-trip is exact,"
flagging `node_named()` as a "one level" lookup to check. Reading
`node_named()` end to end (not skimmed) showed it already walks the
*whole* path component by component against the live `root_dnode` — it
is a complete resolver, not a one-level one, and its contract already
matches `node_absname()`'s exact byte format (it strips `node_absname(
root_dnode)` as a prefix, then `strtok()`s the rest against `NODE_DESC(
node)->name`, the same raw name `node_absname()` itself concatenates).
So `node_from_absname()` is a one-line wrapper around it, kept as a
separate, purpose-named entry point rather than pointing marks straight
at `node_named()` — the two call sites (existing symlink-target
resolution, new mark resolution) can each state their own intent, and
either is free to diverge later without disturbing the other.

**Persistence mirrors `color.c`'s wpattern-group vector round trip
exactly**: `nvs_vector_begin()`/`nvs_path_present()`/`nvs_vector_end()`
around a repeated `mark` node, each holding scalar `name`/`path`
children, under its own `marks` path. Loaded once at startup
(`ui_marks_init()`, the same slot as `color_init()`/`landscape_init()` in
`src/sdl/main.cpp`); every mutation (add/delete/rename) does a full
rewrite immediately (`nvs_delete_recursive()` then re-write the whole
vector) — there is no "Save" button to defer to, unlike the Color Setup
dialog's Apply, so rewrite-on-every-change is the simplest thing that
stays correct.

**UI**: "Mark here" (disabled only when there's no current node — it
doesn't touch the camera, so it isn't blocked by the same in-flight-morph
guard as Reset/Go back/Front view); each row is a label + "Go" + "X". The
label doubles as the smallest-decent-UX inline rename affordance: double-
click to open an `ImGui::InputText`, commit on Enter or on losing focus,
discard an empty edit. A row whose path doesn't resolve shows its label
greyed (`ImGui::TextDisabled`) with a "Not found here: <path>" tooltip
and a disabled "Go" button — never auto-removed, exactly per the brief
("the user might switch back roots"). A resolved row's tooltip shows
`node_absname_display()` (UTF-8-safe, NFC-composed) rather than the raw
stored path, matching the B-era display/storage split; the raw path is
the fallback for a row with no live node to ask a display form of.

**A real bug the brief's own "go-to" verification step surfaced**:
`camera_look_at_full()` asserts the target's immediate parent directory
is already expanded in the dirtree. A mark pointing into a collapsed
subdirectory (the normal case for anything not near the root) hit this
assertion and aborted, discovered by the headed verification below, not
by inspection. Fixed the same way `src/sdl/ui_dialogs.cpp`'s existing
"Look at target node" button already does for symlink targets: `colexp(
target->parent, COLEXP_EXPAND_ANY)` before `camera_look_at()`, if the
parent isn't already expanded. `COLEXP_EXPAND_ANY` walks the *whole*
ancestor chain (`colexp.c` recurses into `dnode->parent` under that
message), so this also covers a mark buried several directories deep,
not just one level — verified directly against the fixture's `dir-a/
dir-b` (two levels under the root).

**Verification.**

1. **Both arms build clean.** SDL/macOS native. GTK: the same Debian
   bookworm container prior tasks used (`fsvbuild`, repo bind-mounted at
   `/work`) — a full reconfigure + rebuild produced the real GTK `fsv`
   executable, **53/53 targets**, including `src/window.c`; `common.c`'s
   new `node_from_absname()` compiled into `libfsvcore` there with no
   unused-symbol warning (it has a real caller: `src/sdl/ui_rail.cpp`,
   which the GTK arm doesn't link, but the function itself is an ordinary
   exported entry point, not `static`, so nothing in that arm's build
   flags to it). `meson test` **4/4 on both arms** (nvstore, scanfs,
   color_persistence, fsn_layout) — `test_fsn_layout.c` gained invariant
   9 (`node_from_absname()` round-trips `node_absname()` for the root, a
   directory nested two deep, a sibling directory, and a file; a
   never-existed path resolves to `NULL`).
2. **Headed, direct-state verification** (temporary, non-committed hooks
   in `src/sdl/main.cpp`/`ui_rail.cpp`/`.h` — the same "SDL_PushEvent /
   direct state where cleaner" allowance prior tasks used, since
   pixel-driving ImGui's double-click-to-rename and button hit-testing
   would test ImGui's own input handling, not this task's logic; deleted
   before either commit below — `git diff` against both shows no trace,
   confirmed by grepping the tree for the hook names after removal). Run
   against `tests/fixture` (`dir-a/dir-b` nested two deep, sibling
   `dir-c`), `$HOME` redirected to a private temp directory so this never
   touched the real invoking user's `~/.fsvrc`:
   - **Add two marks** (`file1.txt` at the root, `dir-a/dir-b`) — logged
     `resolved=1` for both immediately.
   - **Simulate a relaunch** (re-run `ui_marks_init()`'s read path in the
     same process, without touching the in-memory list any other way) —
     logged `count=2`, both still `resolved=1`, proving the add persisted
     to `~/.fsvrc`'s new `marks` vector (inspected directly afterward:
     `marks\n\tmark\n\t\tname ...\n\t\tpath ...`, the same indented shape
     `color`'s own sections use).
   - **Go to each** — logged the resolved target's `node_absname_display(
     )` for both, matching the expected node exactly, and (after the
     `colexp()` fix above) no assertion failure, including for `dir-a/
     dir-b`'s two-level-collapsed case.
   - **Delete one** (`file1.txt`'s mark) — logged `count=1`; simulated
     relaunch again — still `count=1`, only the `dir-b` mark, proving the
     delete persisted too (not just the in-memory erase).
   - **Missing-path case**: with only the `dir-a/dir-b` mark left, Change
     Root to `tests/fixture/dir-c` (a sibling with no `dir-a` in it) —
     logged `resolved=0` for that mark (the greyed-row case) without it
     being removed from the list; Change Root back to `tests/fixture` —
     logged `resolved=1` again, same mark, same path string, never
     re-entered by the user.
3. This section itself, added to `docs/PORTING.md`.

### Concerns / disclosed gaps

- **No de-duplication on "Mark here."** Marking the same node twice
  creates two rows with the same path. Not handled per the brief's "keep
  it simple" instruction; a minor UX rough edge, not a correctness one
  (both rows resolve and behave independently).
- **A missing mark's tooltip shows the raw stored path, not a display-
  safe form.** There is no live `GNode *` for a missing entry to ask
  `node_absname_display()` of; for the vanishingly rare case of a
  non-UTF-8 byte sequence in a stored path, the tooltip could render
  oddly. Accepted rather than duplicating `node_absname_display()`'s
  validate-and-normalize logic against a raw string with no node behind
  it.
- **At most one row can be renamed at a time** (a single `g_editing_
  index`, not per-row state). Matches the brief's "smallest decent UX"
  framing; a second double-click while already editing another row just
  moves the edit to the new row, discarding the first's pending (already
  committed-on-blur, in practice) edit.

### Task C3 verification (double-click opens files, guarded)

Upstream fsn's original "execute or view a file" double-click gesture,
FSN mode only: double-clicking a regular file or symlink hands it to the
system's default opener instead of the ordinary `camera_look_at()`.
Directories, and every special-file type, are unaffected.

Files:

| file | change |
|---|---|
| `src/sdl/input.h` | NEW `OpenFileRequest` seam (mirrors Task 5.1's `ContextMenuRequest`) + `input_take_open_file_request()` |
| `src/sdl/input.cpp` | `node_open_eligible()` (NodeType gate), the double-click branch in `BUTTON_UP`, the matching `impatient_reclick` exemption in `BUTTON_DOWN`, `g_open_file_request` + its `input_reset()` clear |
| `src/sdl/ui_dialogs.h`/`.cpp` | NEW `ui_dialogs_init()`; `draw_open_file_confirm()` (the confirm modal), `open_file_with_system_handler()`, `open_files_allowed` nvstore persistence |
| `src/sdl/main.cpp` | `ui_dialogs_init()` call at startup, alongside `color_init()`/`landscape_init()`/`ui_marks_init()` |
| `tests/fixture/café #1.txt` | NEW fixture file, non-ASCII + space + `#` in the name, for the URL-encoding check below |

#### Security stance

This feature never executes the file's own bytes and never reads its
contents. The double-click resolves a path; `g_filename_to_uri()`
(GLib) turns that into a `file://` URL with the raw filesystem bytes
correctly percent-encoded (spaces, `#`, `%`, non-ASCII); `SDL_OpenURL()`
hands that URL to the OS's own default-application resolver —
LaunchServices on macOS (the exact mechanism Finder's own double-click
uses), `xdg-open` on Linux. Whatever LaunchServices/`xdg-open` decides to
do with the file (which application opens it, and what *that*
application then does) is squarely outside this program's control or
responsibility, exactly as it would be for a Finder double-click — this
program's own obligation ends at handing over a correctly-formed URL.
A symlink is handed to the opener via its own path (`node_absname()`);
the OS resolves the link itself, the same way Finder would. Every
special-file type (FIFO, socket, character/block device) is excluded by
`node_open_eligible()`'s exhaustive switch and simply falls through to
the ordinary `camera_look_at()` — opening one of those has an effect
(blocking on a FIFO with no reader; hardware access for a device node)
a regular file open does not, so this feature declines to touch them at
all.

The first use per node type gets an explicit confirm modal ("Open
`<name>` with the system default app?") naming the file by its display
name; "Always allow" (checked, then Accept) persists past the modal for
future opens (`open_files_allowed`, nvstore, same read-once-at-startup/
write-on-change shape as `color.h`'s `landscape_explicit()`). Cancel is
a hard no-op: no open, no persistence, even if the checkbox was ticked
first.

#### Design decisions

- **The `impatient_reclick` fix was the load-bearing part of this
  task, not a corner case.** `camera.c`'s `FSN_CAMERA_MIN_PAN_TIME`
  (0.5s) routinely outlasts a real double-click's inter-click interval
  — `input.cpp`'s own header comment already documents this for every
  visualization mode's minimum pan time — and the *existing*
  double-click-to-expand toggle already has its own exemption from the
  BUTTON_DOWN "impatient user" discard path for exactly this reason
  (`NODE_IS_DIR(impatient_peek)`). Without the matching exemption added
  here, a real physical double-click on a file would have its second
  press silently discarded before this feature's own BUTTON_UP branch
  ever ran — the feature would work in a synthetic, zero-delay test and
  never fire for an actual user. Caught by tracing the function for
  real during this task's own verification, not by inspection.
- **A true `BeginPopupModal()`, not a plain window** (unlike Properties/
  Color Setup): `imgui.cpp`'s own `io.WantCaptureKeyboard` update goes
  true whenever a modal is open (`(g.ActiveId != 0) || (modal_window !=
  NULL)`, read directly out of the vendored source, not assumed) — so
  `input.cpp`'s Escape-to-collapse handler already bails on its very
  first gate while this modal is open, no change to
  `ui_dialogs_handle_escape()` needed. This app never sets
  `ImGuiConfigFlags_NavEnableKeyboard`, so ImGui's own nav-cancel Escape
  handling never runs either (the same reasoning `input.cpp`'s header
  comment and `ui_main.cpp`'s context-menu popup already rely on) —
  the modal's own body checks `IsKeyPressed(ImGuiKey_Escape)` explicitly
  and calls `CloseCurrentPopup()`, the same pattern that popup already
  uses.
- **Explicit, fixed modal position** (`SetNextWindowPos()` off
  `GetMainViewport()->WorkPos`, the same anchor style `main.cpp`'s scan
  overlay uses), not ImGui's own default placement. A double-click is a
  viewport gesture — the cursor sits directly over the node's own
  on-screen geometry when this fires — and ImGui's default first-use
  placement for an unpositioned window leans on the current mouse
  position, which would put the modal right under the point the user
  just clicked.
- **Paths, not pointers, once anything outlives a frame** — Task B1/C2's
  own discipline: the confirm modal snapshots `node_absname_display()`
  (for the dialog text) and `node_absname()` (for the actual open) into
  owned `std::string`s the instant the request arrives, and never holds
  the `GNode*` itself past that one frame. A rescan while the modal is
  open (unreachable in practice — a true modal blocks the menu bar that
  would trigger one) can therefore never dangle it.
- **`open_files_allowed` lives in `ui_dialogs.cpp`, not `color.h`/
  `color.c`.** Unlike `landscape_explicit()`, nothing outside this file
  — not even the GTK arm, which has no equivalent gesture at all — ever
  needs to ask it, so it stays a local static with its own
  `ui_dialogs_init()` rather than a new core accessor.

#### Verification

1. **Both arms build clean.** SDL/macOS native. GTK: the `fsvbuild`
   Debian bookworm container, full `meson compile`; neither
   `input.cpp`/`input.h` nor `ui_dialogs.cpp`/`.h` is part of the GTK
   arm's source list (SDL-frontend-only files), so the GTK build is
   structurally unaffected — confirmed by rebuilding it anyway.
   `meson test` **4/4 on both arms** (nvstore, scanfs, fsn_layout,
   color_persistence) — this task added no new libfsvcore surface, so
   no new unit test.
2. **Headed, real end-to-end verification** against `tests/fixture`, a
   private `$HOME`, via a temporary headed harness (deleted before the
   commit, same convention as every prior task's "temporary,
   non-committed hooks" — confirmed by `git diff`/grep afterward): real
   `SDL_Event`s fed through `ImGui_ImplSDL3_ProcessEvent()` +
   `input_handle_event()`, exactly the pair the real event loop calls,
   and real `ImGui::Button()`/`Checkbox()` clicks (not direct state
   pokes). Two real bugs surfaced and were fixed as part of chasing this
   verification, detailed below.
   - Double-click on `file1.txt`, **with no synthetic delay at all**
     (click 2 arrives while click 1's own restarted pan is still
     "moving") — modal opens, correct display name
     (`.../tests/fixture/file1.txt`), screenshotted.
   - Escape — modal closes (confirmed via `ImGui::GetDrawData()`'s
     command-list count dropping back toward the base scene once
     closed); re-double-click opens a **fresh** confirm (proving no
     stale request survived).
   - Click **Cancel** — no `SDL_OpenURL` call, no persistence; a
     subsequent double-click still shows a fresh confirm.
   - Click **Open** with the checkbox unticked — `ui_dialogs: open-file
     accepted (always_allow=0)` logged, followed by `ui_dialogs:
     SDL_OpenURL("file:///.../tests/fixture/file1.txt") -> true`;
     `open_files_allowed` still unset afterward (next double-click shows
     the modal again).
   - Tick **"Always allow"**, click **Open** — `accepted
     (always_allow=1)` logged, persisted; a subsequent double-click
     shows **no modal at all**, going straight to a logged
     `SDL_OpenURL()` call — exactly "once always-allowed, no dialog."
   - **Directory regression**: double-clicking `dir-a` (a real
     `NODE_IS_DIR` target, collapsed going in) still toggles it —
     `dirtree_entry_expanded()` false before, true after — unaffected by
     every change above.
   - **MapV regression**: switching to `FSV_MAPV` and double-clicking
     `file1.txt` produces no confirm log, no `SDL_OpenURL` call at all —
     confirmed FSN-only.
3. **URL encoding**: `tests/fixture/café #1.txt` (non-ASCII, a space,
   and a `#`) double-clicked with `open_files_allowed` already true
   (from the step above) logged `SDL_OpenURL("file:///.../tests/
   fixture/caf%C3%A9%20%231.txt") -> true` — `é` correctly percent-
   encoded as its UTF-8 bytes (`%C3%A9`), space as `%20`, `#` as `%23`.
4. **Two real bugs found and fixed while chasing this verification**
   (both disclosed here rather than only in the commit history, per
   this document's own convention):
   - **The `impatient_reclick` exemption** (design decisions, above) —
     without it, a real (non-zero-delay) double-click on a file would
     never have opened anything at all. Caught because the harness's
     *first* pass deliberately fed a double-click with realistic
     zero-gap timing rather than pre-settling the camera, and the
     modal simply never appeared.
   - **A genuine use-after-free in the verification harness itself**
     (not production code): `node_absname_display()` (`src/common.c`)
     returns a pointer into its own reused, `g_free()`'d-and-reallocated
     static buffer; calling it twice as two arguments to the *same*
     `SDL_Log()` call is undefined behavior (argument evaluation order
     is unspecified, and the second call's `g_free()` can dangle the
     first call's pointer before the log line is ever formatted). This
     produced a stray, unformatted `(null)` log line and, via the
     resulting heap corruption, an unrelated-looking
     `camera_look_at_full()` assertion abort several steps later —
     worth recording because it cost real debugging time before the
     actual (harness-only) cause was found. Fixed by snapshotting each
     call's result into its own `std::string` before formatting;
     production code never makes this mistake (nothing else in this
     codebase calls `node_absname_display()` twice in one expression).

#### Concerns / disclosed gaps

- **A pre-existing, unrelated landmine, found by accident while
  building this task's own verification harness**: Task B1 already
  disclosed that a *collapsed* directory still draws its own immediate
  file children as boxes on its pedestal ("indistinguishable from an
  expanded leaf"). Clicking such a file runs the ordinary,
  pre-Task-C3 `camera_look_at()` path — which has no `colexp()`
  pre-expand call the way `ui_rail.cpp`'s Marks "Go" needed one (Task
  C2) — straight into `camera_look_at_full()`'s own `#ifdef DEBUG`
  assertion that the target's parent must already be expanded. In a
  DEBUG build (this one) that aborts the process; in a release build the
  assertion compiles out and the behavior is presumably just wrong, not
  fatal. Reproduced concretely: settling the camera on collapsed
  `dir-a` and picking screen-center lands on `dir-a/file2.bin` (its one
  file child's box), not `dir-a` itself. Out of this task's scope to
  fix — it predates Task C3 and is reachable via the *ordinary* single
  click-to-look-at path, nothing this task added — but worth flagging
  precisely because Task C3 makes double-clicking files a much more
  prominent, deliberate gesture than before. **Update (Milestone C
  final review round): fixed.** Commit `fa9383c` added
  `fsn_ensure_parent_expanded()` in `src/camera.c`, centralized ahead
  of both DEBUG assertions; regression-locked by
  `tests/test_fsn_camera.c` (meson test `fsn_camera`), which also made
  the headless stubs' dirtree section stateful.
- **The confirm modal's fixed corner position never moves once chosen.**
  Fine for this task's scope (YAGNI); a future task wanting a
  cascading/remembered position would need `ImGuiCond_Always` relaxed
  to `ImGuiCond_FirstUseEver`.
- **No de-duplication or queueing of open requests**: a second
  double-click while the confirm modal is already open cannot happen in
  practice (the modal is a true modal — see the design decisions above —
  so the double-click that would create a second request never reaches
  `input.cpp` in the first place; `io.WantCaptureMouse` is true for the
  whole time it's open).

### Fix round (code review): same-drain Esc race + docs sync

Two Important findings, both fixed; two Minors, both resolved (one by a
documented, evidence-backed non-fix).

#### Important 1 — the same-drain Esc race, symmetric with the already-fixed context-menu one

`g_open_file_request` had exactly the gap `g_context_menu_request`
was already fixed for (see this document's own "Post-port additions"
section and `input.cpp`'s header comment): an Escape landing in the
*same* `SDL_PollEvent` drain as the double-click's own `BUTTON_UP` hits
the Escape handler chain before the confirm modal exists.
`io.WantCaptureKeyboard` reflects the *previous* frame's popup-stack
state (computed at the top of `ImGui::NewFrame()`, before this
frame's own draw ever runs), `IsPopupOpen()` is false for the same
reason, and — pre-fix — nothing in the Escape chain knew to ask
`g_open_file_request.pending`. The result: Escape fell through to the
ordinary scene-collapse/step-out logic (an unrelated, *unintended*
side effect on whatever `globals.current_node` happened to be), and
the confirm modal still opened on the very next frame regardless,
because `input.cpp` had already committed the request before the key
event was even processed — "unintended collapse, and the modal pops up
anyway."

**Fix**: `input.cpp`'s Escape handler chain now checks
`g_open_file_request.pending` immediately after the existing
`g_context_menu_request.pending` check, and cancels it the same way —
consumes the keypress, no scene action, request cleared. Exactly
mirrors the pending-context-menu guard; the file header comment's
"Three more gates" list (was "Two") documents the parallel.

**Verification — RED, then GREEN, reproduced directly** (not just
asserted), via a temporary, non-committed harness (`--esc-race-repro`,
removed before this fix's commit — `git diff`/grep confirm no trace):
real `SDL_Event`s fed through `ImGui_ImplSDL3_ProcessEvent()` +
`input_handle_event()` with **no frame boundary at all** between the
double-click's own two clicks and the Escape KEY_DOWN/UP that follows
— the literal same-drain shape.

- **RED** (`git stash` on just the new `g_open_file_request.pending`
  check, rebuilding the pre-fix binary): root directory (expanded
  going in) came out **collapsed** after the same-drain click+Escape
  (`dirtree_entry_expanded()` true → false — the unintended step-out,
  since `globals.current_node` was `file1.txt`, whose parent is root),
  and a frame later the confirm modal's own draw call still produced
  real content (`ImGui::GetDrawData()->CmdListsCount` went from 0 to
  1) — both halves of the reported bug, reproduced concretely, not
  inferred.
- **GREEN** (fix restored): root stayed expanded (no collapse) and the
  modal never rendered any content at all, one frame later or ever —
  the request was cancelled, not merely delayed.
- **Esc with the modal *already* open** (a real frame boundary between
  the double-click and the Escape, not the same-drain race): unaffected
  by the new check — `io.WantCaptureKeyboard` is already true by then
  (the modal has been open at least one full frame), so Escape is
  caught by the pre-existing first gate before the new check is ever
  reached. Confirmed: root stayed expanded, modal closed, matching its
  own unchanged `IsKeyPressed(Escape)` → `CloseCurrentPopup()` body.
- **Ordinary (non-racing) double-click**: confirmed still opens the
  confirm modal normally — the new check does not touch the main
  feature's common-case path at all (it only ever fires when
  `g_open_file_request.pending` is true, which is only ever true in the
  same-drain window this fix targets).
- Both arms rebuilt clean; `meson test` 4/4 on each.

#### Important 2 — docs didn't mention the new gesture at all

`README.md`'s Controls table and its trailing "double-clicking a file…
has no special action" paragraph, and the in-app Help → Controls table
(`src/sdl/ui_main.cpp`), both predated Task C3 and were never updated
for it — both now flatly contradicted the shipped behavior. Fixed:

- `README.md`: new table row ("Double-click a file" → the FSN-mode
  behavior, confirm-then-persist, spelled out; every other mode
  unaffected), the Escape row's note extended to mention the open-file
  confirmation among the popups that get to consume Escape first, and
  the trailing paragraph narrowed to what's actually still true (empty
  space, and files outside FSN mode).
- `src/sdl/ui_main.cpp`'s in-app table: one new row, "Double-click a
  file (FSN mode)" → "Open with the system default app (first use
  asks; Always allow persists)".

#### Minor 3 — a `g_filename_to_uri()` NULL-path fixture

Checked whether a raw invalid-byte filename (the one input that makes
`g_filename_to_uri()` fail on Unix — it also fails for a non-absolute
path, but `node_absname()` always returns an absolute one, so that arm
is unreachable from this call site regardless) is even constructible
as a real fixture file on this task's own macOS/APFS environment:

```
$ touch $'bad\xffname.txt'
touch: bad<0xEF><0xBF><0xBD>name.txt: Illegal byte sequence
```

**Not constructible.** APFS (via the kernel's own filename validation,
not a shell quirk — confirmed the byte reaches the syscall via `touch`
directly) rejects a non-UTF-8 byte sequence in a filename outright, so
there is no way to get such a file onto disk here to double-click in
the first place. `open_file_with_system_handler()`'s `uri == nullptr`
branch (log the `GError` message, free it, return without ever calling
`SDL_OpenURL()`) is therefore verified by code inspection only on this
platform — left that way rather than mocking `g_filename_to_uri()` out
from under real GLib, which would test the mock, not the code. A Linux
CI leg (ext4, which does not validate filename byte sequences at all)
could construct this fixture for real and is the natural place to close
this gap later; noted here rather than silently left unverified.

#### Minor 4 — what "verified" actually meant for `SDL_OpenURL()`

Stated precisely, since the task's own verification bar anticipated a
TCC-restricted sandbox: this environment could confirm `SDL_OpenURL()`'s
**boolean return value** (`true` in every run) and the **exact URL
string** passed to it (correctly percent-encoded — see the café
fixture check above) via `SDL_Log()`. It could not go further with
certainty: a TextEdit process was observed running in this session
(consistent with — though not conclusive proof of — earlier
`SDL_OpenURL()` calls having actually reached LaunchServices), but
enumerating its actual open documents to confirm a specific call opened
a specific window required AppleScript automation
(`osascript -e 'tell application "TextEdit" to get name of every
document'`), which hung waiting on a macOS Automation consent dialog
this non-interactive sandbox cannot answer — the same class of
TCC restriction the task brief itself anticipated. What's verifiable
here, stated exactly: the call path executes and the URL is correct;
whether a window visibly opens on a real, interactive desktop session
is not something this sandbox can independently confirm.

### Task C4 verification (warp-lite fly-in — Milestone C complete)

**What changed.** FSN mode's directory double-click stops being the
plain expand/collapse toggle every other mode still uses. Upstream
fsn's own "warp" flew the camera down onto a clicked pedestal; the
toggle (added post-port, Task 4.1-era, before FSN mode existed) never
distinguished the two. Now: FSV_FSN's double-click branch auto-expands
a *collapsed* target (`colexp(COLEXP_EXPAND)`, letting the deployment
morph run *during* the fly-in — the pedestal's box grid rises while the
camera is still travelling toward it) and calls a new
`camera_warp_to()` instead of `camera_look_at()`. An *already-expanded*
target — including one already warped into — never collapses on a
second double-click: warp was never a toggle upstream either, so the
branch simply has no collapse call in it at all, and a re-click just
re-centers (a visible no-op if the camera is already there). Collapsing
an FSN directory stays reachable via Escape, the context menu, or the
panel's tree-row arrow — this task narrows one gesture, not the whole
feature set. Every other mode's double-click is untouched: the
pre-existing toggle code is preserved verbatim, gated behind an
`else if` that only fires when the FSN branch's own
`globals.fsv_mode == FSV_FSN` guard is false.

**Files.** `src/camera.c`/`.h` (`camera_warp_to()`, `fsn_warp_pose()`,
and the `camera_pan_begin()`/`camera_pan_commit()` prologue/epilogue
factored out of `camera_look_at_full()` so the two share it rather than
duplicating it); `src/fsn-style.h` (`FSN_WARP_PHI`,
`FSN_WARP_HEIGHT_LIFT`, `FSN_WARP_DIAMETER_FRAC`); `src/sdl/input.cpp`
(the FSN-only branch in `BUTTON_UP`, and its header comment).

**The refactor camera_warp_to( ) needed first.** `camera_look_at_full()`
already has the exact hook pattern a warp needs — end a flight, drop
into bird's-eye view if active, save scroll state, break any pan in
progress, then (after the pose is computed) arm the master pan morph
and update history/current-node bookkeeping. Duplicating that ~25-line
prologue/epilogue for one more entry point is exactly the kind of
copy-shaped state this codebase's other tasks have refactored away
(B2's `cancel_pan_for_manual_control()`, the C1/C2 accessor-not-copy
pattern) — so it is now two static helpers,
`camera_pan_begin()`/`camera_pan_commit()`, and
`camera_look_at_full()` and `camera_warp_to()` both call them.
`camera_warp_to()` has no per-mode switch the way
`camera_look_at_full()` does: it is FSN-only by construction (the only
caller is gated on `globals.fsv_mode == FSV_FSN`), so
`fsn_warp_pose()` — shaped exactly like the pre-existing `fsn_look_at()`
— stands in for the switch's one live arm. `camera_warp_to()` also
takes no `MorphType`/`pan_time_override`: every caller wants the same
`MORPH_SIGMOID` landing, so unlike `camera_look_at()`'s thin wrapper
around `camera_look_at_full()`, there is nothing for a caller to
override, and no need to require `animation.h` (which
`camera_look_at_full()`'s declaration is `#ifdef`-gated on) at the
call site in `input.cpp`.

**The landing pose.** `fsn_warp_pose()` reads the same `FsnPedestal` as
`fsn_look_at()` (`fsn_layout_get()`) and computes the same spherical
`target + distance · (θ, φ)` camera position, but framed tight on the
one pedestal instead of the whole landscape:

- **Target**: the pedestal's own center (`x`, `z`), raised
  `FSN_WARP_HEIGHT_LIFT` (110 units) above its top (`h`) — aiming at
  roughly file-box height rather than the bare pedestal surface
  underneath them. `fsn_warp_pose()` has no per-child geometry, only
  the parent `FsnPedestal`, so this is a fixed guess at "typical" box
  height (between `FSN_BOX_H_MIN` 16 and `FSN_BOX_H_MAX` 320), not a
  per-box lookup.
- **Elevation** (`FSN_WARP_PHI`, 18°): higher than `camera.c`'s own
  `FSN_CAMERA_PHI` (15°, that constant's grazing establishing-shot
  pitch for the whole-landscape overview). A first pass tried 8°
  with an *unraised* target and produced, confirmed by screenshot, a
  camera standing in the aisle between two rows of boxes staring down
  a canyon of their side walls — the opposite of "file boxes fill the
  view". Raising both the target (above) and the elevation together is
  what clears the camera over the box canopy instead of threading it
  between two rows.
- **Distance** (`FSN_WARP_DIAMETER_FRAC`, 0.22 of the pedestal's own
  `MAX(w, d)`, vs. `fsn_look_at()`'s `SQRT_2 · MAX(w, d)` framing the
  *whole* footprint): close enough that a handful of boxes dominate the
  frame with real perspective, the way the reference screenshot (cited
  by `FSN_CAMERA_PHI`'s own comment, and again below) shows a handful
  of large near boxes and two more pedestals with converging wires
  receding into the distance.
- **Pan time and theta unwrap**: identical formulas to `fsn_look_at()`
  — travel-proportional duration (`FSN_CAMERA_MIN/MAX_PAN_TIME`), and
  `unwrap_theta_toward()` before arming the morph (the B2 lesson: theta
  is an angle, `morph()` interpolates a number, so a viewer parked just
  past the wrap point must have theta shifted by a whole turn first or
  the pan spins 267° the long way instead of turning ~90° the short
  one).

**Verification.**

1. **Both arms build clean.** SDL/macOS native (`ninja -C builddir-sdl`)
   and the `fsvbuild` Debian bookworm container's GTK arm
   (`ninja -C builddir-gtk`) — `camera.c`/`.h` and `fsn-style.h` are
   shared core, `input.cpp` is SDL-only. `meson test`: **4/4 on both**
   (nvstore, scanfs, fsn_layout, color_persistence) — no new
   `libfsvcore` surface (the warp pose lives in `camera.c`, which links
   into every existing test binary unchanged), so no new unit test.
2. **Headed, real end-to-end verification**, same convention as Tasks
   B2/C3: a temporary, env-var-gated harness (`FSV_C4_VERIFY`) added to
   `src/sdl/main.cpp`, pushing real `SDL_Event`s through
   `ImGui_ImplSDL3_ProcessEvent()` + `input_handle_event()` — the exact
   pair the real event loop calls — fully removed before the commit
   (`git diff`/`git status` on `main.cpp` after removal show **zero net
   change** to that file). Run against this repo's own `src/` tree
   (`fsv src --fsn`), targeting `root_dnode` itself (always
   front-and-center in FSN's intro framing, sidestepping having to hunt
   for a child pedestal's exact screen position):
   - **Auto-expand + swoop**: root force-collapsed
     (`dirtree_entry_expanded()` 1→0), then a real double-click (two
     manually click-numbered press/release pairs, one push per
     animation tick — see the next point) at the pedestal's screen
     position: `expanded` flips 0→1 *before* the pan even finishes
     (`dirtree_entry_expanded()` flips synchronously the instant
     `colexp()` starts, confirmed mid-pan), and the camera lands at
     `theta=270 phi=18 dist=147.8` — `FSN_WARP_PHI`, and a travel
     distance an order of magnitude tighter than the ~950–2369 unit
     distances the plain establishing shot uses for the same tree.
     Screenshot pair `c4_before.png`/`c4_after.png` (task's
     `screenshots/` directory): the before frame is the familiar
     head-on collapsed-root view (root's own file boxes, modest,
     `src` labeled on the ground); the after frame is a close, elevated
     view with a handful of large file boxes filling most of the frame
     and two child pedestals with converging wires visible in the
     distance — read directly and compared against the task's
     reference image (`35037135976_0d90f4a3d5_z.jpg`): both show the
     same "standing among the files" composition — sky band at the
     top, a grid of boxes filling most of the lower frame, wires
     converging toward pedestals further back. Not pixel-identical
     (different tree, different box count) but the same camera language.
   - **A genuine harness bug, caught and fixed before it could produce
     a false negative**: pushing all four button events of a
     double-click in one `SDL_PushEvent()` burst made the *second*
     click's own `BUTTON_UP` see a stale `camera_moving() == true` and
     silently no-op — not a bug in this task's code, but in
     `camera_pan_finish()`'s own documented contract.
     `morph_finish()` (`animation.c`) only sets `Morph::t_end` to 0.0;
     it does not synchronously clear `camera_currently_moving`, which
     only happens on the *next* `fsv_animation_tick()`. A real physical
     double-click always has several ticks between its own press and
     release (even a fast one), so this never surfaces outside a
     zero-delay synthetic harness — but a zero-delay burst reproduces
     it every time. Fixed in the harness (one button event pushed per
     animation tick, matching a real click's own timing shape), not in
     production code, since production code has no such burst path.
   - **Re-double-click, deliberately mid-pan (rapid re-click, the
     "warped in" case)**: a second double-click fired one tick after
     the first, while the first's own pan was still in flight
     (`impatient_reclick`'s dir exemption — pre-existing, verified
     still applies unconditionally on `NODE_IS_DIR()`, unmodified by
     this task — is what lets the second click's `BUTTON_DOWN` pick
     instead of being discarded as "impatient user"). Result:
     `expanded` stays **1** throughout (`0→1→1`, never back to 0) and
     `current_node` never changes — no collapse, exactly the "state
     chosen behavior" the brief asks for. `camera_pan_break()` inside
     `camera_pan_begin()` cleanly cancels the first warp's just-armed
     morphs and re-arms fresh ones to the same destination — the same
     "second click while a pan is already running" pattern the rest of
     this codebase already relies on (B2's flight-during-a-pan, the
     ordinary ordinary-second-click-on-a-file case), not a new race.
   - **Escape still collapses**: with the pedestal warped into and
     expanded, a real `Escape` key event collapses it
     (`dirtree_entry_expanded()` 1→0) — the toggle-adjacent behaviors
     this task deliberately left alone (Escape/context-menu/panel
     collapse) are all still live.
   - **Flight interrupted by a warp, then the swoop lands cleanly**: a
     real middle-button press + forward-drag starts a flight
     (`camera_flight_active()` reads 1), then a double-click on the
     (freshly re-collapsed, via the Escape step above) target fires
     *while the middle button is still held*. Result:
     `camera_flight_active()` reads **0** after the middle button is
     released and the pan settles, and `expanded` reads **1** — the
     flight ended (via `camera_pan_begin()`'s `camera_flight_end()`,
     the same hook `camera_look_at_full()` already relies on for this)
     and the auto-expand + swoop ran to completion, not a partial or
     corrupted state. No new pending seam is introduced by warp (it is
     a direct, synchronous camera call from `input.cpp`, unlike the
     C3-era `g_open_file_request`/`g_context_menu_request` structs), so
     there is no new same-drain Esc race to guard — confirmed by
     inspection: `camera_warp_to()` takes effect within the same event
     that calls it, with nothing deferred to a later frame for Escape
     to race against.
   - **Wrapped theta, short arc**: rather than fly a real yaw to the
     wrap point (B2's own, slower, verification), `camera->theta` was
     poked directly to 3.0 and `camera_warp_to()` called directly (the
     exact same production entry point `input.cpp` calls, just without
     the picking layer in between — picking was unusable for this one
     check, see the next bullet) targeting the already-expanded,
     already-current root. Result: `camera->theta` reads **363.0**
     immediately after the call returns — `unwrap_theta_toward()`
     shifted it by a whole turn *in place*, synchronously, before
     arming the morph — and `|363 − 270| = 93 ≤ 180`, the short arc,
     matching B2's own fix-round verification pattern for
     `fsn_look_at()` exactly (this is the same fix, exercised in the
     new function it also needed).
   - **A second harness-methodology bug, caught by tracing rather than
     assumed**: the *first* attempt at the wrapped-theta check used a
     simulated click at the pedestal's *pre-warp* screen position — but
     by that point in the script the camera had already warped in
     close, and that same screen pixel now landed on one of the
     pedestal's own *file*-box children instead of the pedestal itself
     (exactly the "boxes fill the view" effect this task built,
     working as intended). The resulting double-click hit Task C3's
     file-open path instead of a re-warp, and the file-open confirm
     modal it opened then stayed open and captured
     (`io.WantCaptureMouse`) every subsequent synthetic click for the
     rest of the script, silently invalidating the MapV-regression
     check that ran after it. Diagnosed by adding, then removing,
     temporary `SDL_Log()` probes in `input.cpp`'s `BUTTON_DOWN`/
     `BUTTON_UP` cases (`io.WantCaptureMouse`, the resolved node, and
     `camera_moving()`) — confirmed no trace of those probes remains
     (`git diff`/`git status` clean on `input.cpp` beyond this task's
     real change). Fixed by calling `camera_warp_to()` directly for
     this one check instead of through a simulated click, as above.
   - **MapV regression**: mode-switched to `FSV_MAPV`. The double-click
     branch this task changed is unconditionally FSN-only
     (`&& globals.fsv_mode == FSV_FSN`); every other mode falls to the
     pre-existing `else if (NODE_IS_DIR(...))` toggle, byte-identical
     to the code before this task. The harness's own synthetic click at
     the same fixed screen pixel landed on a MapV-mode file box rather
     than the root node in that mode's different (stacked, top-down)
     layout — a pre-existing picking-target-precision limitation of a
     fixed-pixel synthetic click, not a code path this task touches —
     so this regression is verified primarily by the code diff itself
     (one added `&&` condition gates the new branch; the toggle branch
     below it is untouched) rather than a clean empirical re-run;
     disclosed rather than glossed over.
   - **FSN file double-click (Task C3) regression**: mode-switched back
     to FSN, centered on an actual file sibling of root
     (`geometry.c`), real double-click. `input_take_open_file_request()`
     reads `pending == 1` — the file-open path is untouched and still
     fires for files, confirming the new dir-only branch's placement
     (ahead of, and mutually exclusive with, the pre-existing file-open
     `else if`) doesn't shadow it.
3. **Idle CPU unchanged.** `camera_warp_to()` and `fsn_warp_pose()` run
   only synchronously inside `input_handle_event()`'s `BUTTON_UP` case,
   adding no per-tick main-loop cost the way B2's flight tracking did
   (`camera_flight_tick()`) — there is nothing new for an idle frame to
   pay for. Measured anyway: process CPU time over a 10 s idle window
   in FSN mode, this branch's binary, **0.07 s** — consistent with
   B2's own idle baseline (0.11–0.12 s) and B2's explanation of where
   that number comes from (the animation subsystem's own per-tick cost,
   unrelated to this task).

**Concerns / disclosed gaps.**

- **`FSN_WARP_HEIGHT_LIFT` is a fixed guess**, not a per-directory
  computation from its children's actual box heights — `fsn_warp_pose()`
  only has the parent `FsnPedestal`, and a real per-child lookup would
  need a second, more invasive geometry-accessor addition for a purely
  cosmetic gain. Tuned by iterating three real screenshots against the
  task's reference image rather than picked once and trusted.
- **The MapV regression check** (above) is verified by code diff, not
  a clean empirical click-through, due to a synthetic-harness picking
  limitation in that mode's layout at the one fixed pixel this
  harness used — disclosed rather than re-run indefinitely to chase a
  harness-only precision issue on a code path this task does not
  modify.
- **No Search panel, no true in-directory paradigm** — explicitly out
  of scope per the task brief (YAGNI). Warp-lite is a camera pose and
  an auto-expand, nothing more; upstream fsn's full warp UI state is
  not ported.
- **A `colexp()`-internal repan can be discarded by the warp it raced
  with.** `colexp(COLEXP_EXPAND)`'s own depth-0 epilogue may arm a
  `camera_look_at_full()` re-pan of its own (when `globals.current_node`
  is an ancestor of — or equal to — the node being expanded, per
  `colexp.c`'s `curnode_is_ancestor` check); `input.cpp`'s
  warp-lite branch calls that `colexp()` and then, in the same event,
  `camera_warp_to()` — whose `camera_pan_begin()` unconditionally calls
  `camera_pan_break()`, cancelling whatever pan is in flight, including
  one `colexp()` itself just started. Benign (the warp is the pan the
  user actually asked for, and it lands at the intended target either
  way) and precedent-consistent — the "second click while a pan is
  already running" pattern the rest of this codebase already relies on
  (B2's flight-during-a-pan, an ordinary second click on a file) is
  exactly this shape, just with the first pan started one call deeper.

Not pushed, per the global constraints.

## Post-v0.3: MapV squarify + scan exclusion (2026-08-14)

Closes two 1999-upstream `TODO` items (see `TODO.md`), independent of
the fsn-mode work above. Three changes: (1) `src/squarify.c`, a pure
squarified-treemap module (Bruls et al. 2000) replacing `geometry.c`'s
old greedy row layout — no more paper-thin frontmost rows; (2) a
√size default area scale (linear/log₂ as Display-menu alternatives,
persisted, loaded pre-scan in `ui_dialogs_init()`); (3) `scanfs.c`'s
built-in, conservative exclusion list (**exact basename match only** —
`.git`, `builddir`, `node_modules`, etc. — dirs only, no prefix/glob
matching, so differently-named build output such as `builddir-sdl`,
`build/`, `target/`, or `dist/`, and other dot-directories such as
`.superpowers`, still scan normally), on by default with a Vis-menu
toggle. Two new tests (`squarify`, `scanfs_exclude`), suite now 8/8.
GTK arm gets the same core defaults (squarify, √size, exclusion-on)
with no dedicated GTK UI added for any of the three — the Vis toggle
and Display option are SDL-frontend-only, matching this port's
existing pattern of core-first, frontend-as-needed. Since the GTK arm
has no toggle, it has no way to turn exclusion off from a menu either;
`scanfs.c`'s `dir_name_excluded()` also honors an `FSV_NO_EXCLUDE`
environment variable (any value disables exclusion for the process),
which is that arm's escape hatch. Design:
`docs/superpowers/specs/2026-08-14-mapv-squarify-scan-exclude-design.md`.

**Deviation from the design spec (I3):** §2 of the design spec called
for MapV's root dimensions to switch to the weighted area scale along
with everything else. `490936f` deliberately kept them byte-based
(`DIR_NODE_DESC(...)->subtree.size` in `mapv_init()`, not
`area_weight`) instead: the root's absolute world size feeds directly
into camera framing and the fixed `mapv_dir_height`/`mapv_leaf_height`
constants (384/128), both tuned against byte-scaled worlds since
upstream fsn, and weights are relative-only by design (squarify
normalizes areas internally regardless of their absolute scale). The
companion consequence of that choice is the weight→world unit
mismatch the *Final-review fix wave* below (C1) had to correct:
because the root stays byte-scaled while every level below it now
plots at the current area scale (√size by default), a per-node
constant like `nominal_border` — a world-space length — no longer
lived in the same units as the weight-space quantities it was
being added to, and `mapv_init_recursive()` needed an explicit
weight→world pre-scale to reconcile the two.

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
| 2026-08-07 | glslangValidator + spirv-cross over `sdl3_shadercross` for offline shader compilation | no Homebrew formula for `shadercross`/`sdl3_shadercross` exists; the glslang/spirv-cross pair is the brief's own documented fallback and worked cleanly end to end |
| 2026-08-07 | `normal_matrix` passed as `mat4` (not `mat3`) in the scene vertex UBO | std140 packs `mat3` as three ambiguously-padded vec4 columns; `mat4` removes the ambiguity between the GLSL and future C++ (`gpu.h`) struct layouts, at the cost of one wasted row/column |
| 2026-08-07 | Embed compiled shaders in the binary (`tools/embed-shaders.py`) instead of loading them from disk | one binary has to run from a build tree, an install prefix and an `.app` bundle; a runtime path lookup adds three startup failure modes for no benefit. Matches the vendored ImGui backend |
| 2026-08-07 | Two render passes per frame (scene then ImGui) rather than one | ImGui must not be depth-tested and must not inherit/leak the scene's viewport+scissor state; costs one extra Metal encoder |
| 2026-08-07 | `glm_frustum_rh_zo()` per call site, not a project-wide `CGLM_CLIP_CONTROL` | SDL_GPU wants `[0,1]` depth while the still-OpenGL GTK frontend shares cglm and wants `[-1,1]`; a global switch would silently retarget it |
| 2026-08-07 | Separate "id" pipeline for picking, but no separate shader | a pipeline's color-target format is fixed at creation and picking reads back from an offscreen `R8G8B8A8` texture, not the swapchain; the flat id color is just a uniform push, exactly as `geometry.c` already does in `RENDERMODE_SELECT` |
| 2026-08-07 | `gpu.h` lives in `src/`, not `src/sdl/` | it is the contract the shared core draws against, implemented once per frontend (SDL_GPU and the epoxy shim), not an SDL-private header |
| 2026-08-07 | Immediate-mode `gpu_draw()` recorded + replayed, instead of retained `FsvMesh` handles | geometry.c owns no persistent meshes; SDL_GPU forbids copies inside a render pass, so uploads must precede the draws that consume them, which a per-call-site mesh reused across nodes cannot satisfy |
| 2026-08-07 | Expand fans/strips/loops into indexed TRIANGLELIST/LINELIST rather than add strip pipelines | halves the pipeline count and costs only integer appends over data already being copied; no strip pipeline could have served the fan or the loop anyway |
| 2026-08-07 | Splash screen + "fsv" logo move from geometry.c to about.c | they are the only geometry drawn through the separate *about* shader program, which Task 3.1 never ported; keeping them would force a second pipeline into the shared gpu.h contract that only GTK could implement |
| 2026-08-07 | Accept 1-pixel lines on SDL_GPU instead of emulating `glLineWidth` with quads | SDL_GPU has no line-width control on any backend; the cursor reads thinner than on GTK, which is a cosmetic difference not worth a quad-expansion path |
| 2026-08-07 | Port `tmaptext.c` in place behind six new `gpu.h` entry points, no `src/sdl/text3d.cpp` | its GL surface is small (55 calls: one texture, one program, one draw) against a lot of shared glyph-layout math; splitting into a second file would have duplicated that math or `#include`d the original, both worse than extending the contract Task 3.3 already established for `geometry.c` |
| 2026-08-07 | Text pipeline: depth write ON, matching the GL original exactly, not the "typical" depth-write-off pattern for text overlays | `grep -n "glDepthMask" src/*.c` is empty everywhere in the codebase -- the old renderer never toggled it, so text always drew with GL's default (write enabled), same as scene geometry; matching reality was the brief's own instruction, and drawing all text after all scene geometry in one pass (see "Recording and replay") makes write-order moot anyway |
| 2026-08-07 | Text draws get their own second recording arena (`g_text_vertices`/`_indices`/`_draws`), replayed after the scene's within the same render pass | different vertex format and pipeline from the scene (`FsvTextVertex` vs `FsvVertex`, alpha-blended+textured vs opaque+lit) rule out reusing the scene's `DrawCmd`/arena; a separate copy pass and transfer buffer is one more small allocation per frame in exchange for zero coupling between two arenas of very different size and churn |
| 2026-08-07 | Ported viewport.c's *actual* gestures (middle-drag dolly, Ctrl+left-drag revolve), not the brief's assumed ones (middle-drag "flight", Shift for vertical) | read in full: viewport.c has no flight mechanic and no Shift modifier anywhere; porting an imagined gesture instead of the real one would silently diverge from upstream behavior |
| 2026-08-07 | Scroll-wheel dolly is a labeled addition, not a port | viewport.c/doc/mouse.html have no scroll-wheel gesture at all; the brief and the verification bar both ask for one explicitly, so it is wired using the same `camera_dolly()` entry point and sensitivity shape as the middle-drag case rather than left unimplemented |
| 2026-08-07 | Explicit `SDL_CaptureMouse()` on middle/Ctrl+left press despite SDL3's own mouse auto-capture already covering it | makes the intent self-documenting in the source rather than resting silently on a hint (`SDL_HINT_MOUSE_AUTO_CAPTURE`) whose default a user or future change could flip |
| 2026-08-07 | `viewport_node_for_id()` added to the existing `src/sdl/stubs.c`/`src/viewport.h`, not deferred to Task 4.2 | the node table itself already lives in `stubs.c` (Task 3.3); Task 4.2 only has to make `gpu_pick()` return a real id, not build a second lookup path |
| 2026-08-07 | `gpu_pick()` reuses `gpu_screenshot_*()`'s `g_capture_texture` redirect instead of a second offscreen-target mechanism | `gpu_scene_begin()` already keys `g_color_target`/`g_target_index` off `g_capture_texture`; a second mechanism would duplicate that branch for no benefit — the two callers (screenshot, pick) never run concurrently |
| 2026-08-07 | `gpu_scene_end()`'s clear color branches on `g_render_mode == FSV_RENDER_SELECT` rather than giving picking its own render-pass function | the dark-slate clear (Task 2.2) decodes as a non-zero, bogus node id for any pixel the id pass draws nothing over (e.g. empty sky); one branch on already-shared state was simpler than a parallel copy of `gpu_scene_end()` |
| 2026-08-07 | `gpu_pick()` acquires its own `SDL_GPUCommandBuffer` rather than piggybacking on the main loop's | `input_handle_event()` always runs before that iteration's `gpu_frame_begin()`, so `g_cmd` is provably null at call time; a dedicated command buffer keeps the pick's copy pass and fence wait from ever touching the frame the user is about to see |
| 2026-08-07 | `draw_lit()`'s fixed-color path draws id 0 (black) in the select pass instead of skipping the draw | skipping would remove TreeV's branch/loop connectors from the pick pass's depth buffer, letting a click pass through to whatever node sits behind them — pick-pass occlusion has to match the visible scene; id 0 keeps the occlusion while correctly reporting "not a node" |
| 2026-08-07 | Rescan/Change Root are *requested* synchronously from `ui_main.cpp` but *applied* (i.e. `scanfs()` actually runs) only after `main.cpp`'s loop closes out the current frame | `scanfs()`'s own progress overlay (`gui_update()`) drives its own `ImGui::NewFrame()`/`Render()` pair; running it from inside `ui_main_draw()` would re-enter `ImGui::NewFrame()` while the main loop's own pair (which is what called `ui_main_draw()`) is still open |
| 2026-08-07 | `app_switch_mode()`/`app_root_dir()`'s int/`const char *` boundary keeps `FsvMode` as a plain `int` rather than including `common.h` a second time | `common.h` self-guards against a second `#include` in the same translation unit (`#ifdef FSV_COMMON_H #error`) and every caller already includes it; matches `input.h`'s existing `void *` treatment of `GNode *` for the same reason |
| 2026-08-07 | `load_filesystem()` stores `xgetcwd()`'s result (post-`scanfs()`), not the caller's `dir` argument, as the tracked root | `scanfs()` `chdir()`s into `dir` and never `chdir()`s back (`src/scanfs.c:320`); storing the caller's string verbatim would hand Rescan a stale, possibly-relative path that resolves against the *new* working directory on the next call — confirmed by reproducing the resulting fatal `g_error()` (`"Failed to change dir to src"`) before this fix and its absence after |
| 2026-08-07 | Context-menu seam (`input.cpp` writes, `ui_main.cpp` reads) mirrors `viewport_node_for_id()`'s one-way pattern rather than a callback | keeps `input.cpp` ignorant of ImGui beyond the `WantCaptureMouse` check it already made; `ui_main_draw()` polls once per frame instead of `input.cpp` needing a function pointer into a file that doesn't exist yet when `input.cpp`'s API was designed (Task 4.1) |
| 2026-08-07 | Full-screen `screencapture` in this sandbox returns a solid-black image regardless of window state | unlike prior tasks' "window occluded" finding, this session's virtual display has no capturable compositor output at all (confirmed capturing the whole screen, not just the fsv window); ImGui-overlay pixels (menu bar, popups, About/Controls windows) could not be visually verified this way, only via internal-state tracing plus the (unaffected) offscreen scene-only `gpu_screenshot_*()` path |
| 2026-08-07 | `ContextMenuRequest` carries only logical coordinates (`win_x/win_y`), not a second pixel-space field alongside them | the pick has already happened (`input.cpp` resolved `node` via its own pixel-space locals) by the time the struct is filled, so nothing downstream ever needs pixel space again; a second field would be dead weight inviting the next person to reach for the wrong one |
| 2026-08-07 | Verified the Retina/HiDPI coordinate fix with a temporary, reverted `FSV_TEST_DENSITY` env-var override in `pixel_scale()` rather than trusting the sandbox's real (1.0) display density | at 1x, logical and pixel coordinates coincide numerically, so a same-density-only re-test could not have told the pre-fix and post-fix code apart; forcing 2.0 made the two spaces provably diverge and confirmed the popup tracks the logical pair, not the pixel one, in both cases |
| 2026-08-07 | Repurpose `DirNodeDesc::tnode` as a plain expanded/collapsed flag instead of adding a new field | `scanfs.c` only ever `NULL`s it once before the first `dirtree_entry_new()` call and never reads it back; it exists specifically as "the frontend's per-directory tree-widget handle", which is exactly what this flag is, just for a frontend with no persistent widget to hold a handle to. *Addendum (Milestone C final review round):* `tools/fsv-headless-stubs.c`'s dirtree section now shares this same repurposed flag as a second consumer, mirroring `src/sdl/ui_panels.cpp`'s semantics so `tests/test_fsn_camera.c` can exercise `src/camera.c`'s real expand/collapse state |
| 2026-08-07 | Directory tree panel keeps `GNode->children`'s structural (dir-first, size-descending) order rather than re-sorting to match `dirtree.c`'s alphabetical insertion order | that order is geometrically significant (MapV/TreeV/DiscV layout depends on it), so it cannot be mutated; re-deriving an alphabetical view every frame for potentially large directories would cost real time for a cosmetic-only match. The file list *is* sorted alphabetically, matching `filelist.c` exactly, because it only re-sorts a cached copy on a directory change, not per frame |
| 2026-08-07 | `dirtree_entry_expand()`/`_expand_recursive()` walk up to open every ancestor, a step GTK's own versions never needed | `GtkTreeStore` rows all exist regardless of expansion (collapsing only hides children); this file's tree walk only descends into rows already known to be open, so a closed ancestor would make the target permanently undrawable no matter its own flag |
| 2026-08-07 | Verified the panel↔3D sync end-to-end with real `SDL_PushEvent`-injected input (motion, then press, then release as three separately-timed steps) rather than direct state calls | direct calls would only prove the notification plumbing, not that a real click on the actual rendered widget reaches it; the three-step split was required after bundling press+release in one frame silently failed `TreeNodeEx()`'s `OpenOnArrow` toggle (`ImGuiButtonFlags_PressedOnClick`) despite `IsItemClicked()` still reporting true |
| 2026-08-07 | `io.IniFilename` set to an `SDL_GetPrefPath()`-derived absolute path rather than left at ImGui's cwd-relative default | `scanfs.c` permanently `chdir()`s into whatever directory was last scanned; a relative `imgui.ini` would follow it there instead of living anywhere stable, and would relocate itself on every Rescan/Change Root |
| 2026-08-07 | Default dock layout seeded via `DockBuilder*` gated on the resulting node's `IsEmpty()`, not on whether `imgui.ini` exists on disk | `DockSpaceOverViewport()` already resolves/creates the node before the gate can run either way; checking the node's *own* structure (already restored from a prior `imgui.ini` load, if any, by the time this code runs) is what actually distinguishes "nothing to preserve" from "a user's layout exists", not a filesystem check that can't see what ImGui already loaded into memory |
| 2026-08-07 | `ImGuiListClipper::IncludeItemByIndex()` before the first `Step()`, not a second, unclipped render pass | it is the one mechanism `imgui.h` documents for exactly this need (force a specific, possibly off-screen index to be processed at all) and composes with the clipper's own multi-pass `Step()` loop already in place, instead of bypassing clipping (and its perf benefit) entirely whenever any scroll-to is pending |
| 2026-08-08 | Xcode target is a `PBXLegacyTarget` wrapping `meson`/`ninja`, not a native target compiling the sources itself | keeps Meson the single source of truth for the build graph; a native target would require mirroring every `meson.build` rule (subproject, embedded shaders, per-frontend gating) inside the pbxproj too, doubling the maintenance surface for zero benefit |
| 2026-08-08 | Ad-hoc `codesign` lives in `packaging/macos/make-bundle.sh`, not an Xcode build phase | an External Build System target has no product/Signing tab for Xcode to drive itself; a Run Script phase bolted onto a phase-less legacy target would be exactly the "fake compile phase" the task's own constraints rule out, and the script is useful standalone (CLI-only users, CI) |
| 2026-08-08 | `config.h` generates into `<builddir>/src/config.h`, not the build root; `incdir`'s bare `'..'` entry removed | that bare `'..'` put the repo root on every C/C++ compile's include path so `config.h` (at the build root) was reachable; libstdc++'s `<bits/stl_algobase.h>` unconditionally `#include <debug/debug.h>`, and with the repo root ahead of the real system path, that silently resolved to this project's own `debug/debug.h` instead, breaking every C++ (SDL frontend) compile on Linux/GCC — never caught before Task 6.2 because no prior task had actually built `src/sdl/*.cpp` on Linux |
| 2026-08-08 | Build SDL3 from source (CMake, cached by `actions/cache`) for the Linux CI job, rather than `apt install` or a non-LTS Ubuntu runner | `libsdl3-dev` doesn't exist for `jammy`/`noble` in Ubuntu's archive (only from `questing` 25.10 onward), and GitHub only hosts LTS-base Ubuntu runners (22.04/24.04) — there is no package to install on any GitHub-hosted Ubuntu image today |
| 2026-08-08 | `linux-sdl` CI job is `continue-on-error: true`; `release` explicitly ignores its result (with `always()`) rather than gating on it | building SDL3 from source on Ubuntu is inherently more fragile than the apt-packaged GTK path and is a bonus, not the anti-regression job; releases must still ship a working Linux binary (falling back to GTK) even if this job fails |
| 2026-08-08 | CI's `linux-sdl` job links SDL3 statically (`-DSDL_SHARED=OFF -DSDL_STATIC=ON`), not shared, and gained a standing `ldd`-based regression check for it | a shared build would dynamically link `libSDL3.so`, which no Ubuntu release ships a runtime package for either — the release binary would fail to start for every downloader with no user-side fix; static removes the dependency entirely, confirmed by running the binary in a bare `ubuntu:24.04` container |
| 2026-08-08 | `make-bundle.sh` accepts a direct binary path as an alternative to a meson builddir | the script previously only understood a builddir layout, so it always failed when bundled into a release tarball, where the binary sits flat next to it instead |
| 2026-08-08 | README's Controls table describes the real gestures read from `src/sdl/input.cpp`, not the task brief's own pre-reading assumption ("double-click activate/warp") | there is no double-click action anywhere in the 3D viewport, in this port or upstream — `viewport.c`'s original code and this file's port of it both treat a double-click as two ordinary clicks; porting an imagined gesture into user-facing docs would misinform users about behavior that doesn't exist. **Superseded 2026-08-08 by a post-port addition** (double-click-to-expand a directory) — see "Post-port additions" below and this same table's Double-click row above; the claim was accurate for the Task 6.5 state of the code and is left as a historical record rather than rewritten |
| 2026-08-08 | Old "Misc notes / OpenGL versions" section kept verbatim, moved under a collapsed `<details>` rather than deleted | it documents real, still-true constraints on the GTK/OpenGL frontend (core-profile context negotiation, GLSL version floor), which this port didn't touch and doesn't obsolete |
| 2026-08-08 | Purge the morph/scheduled-event queues at the *top* of `scanfs()`, before the frees, rather than at `dirtree_clear()` | the queues are what is being emptied, not the tree, so doing it first means they never briefly hold pointers to freed memory; it also makes the purge unconditional instead of dependent on `globals.fstree != NULL` |
| 2026-08-08 | `morph_break_all()` and `scheduled_events_clear()` kept as two functions, not one | two independent queues with two independent public entry points (`morph_full()`, `schedule_event()`); one name cannot honestly describe both, and the only caller wants both adjacently anyway |
| 2026-08-08 | Hover picks coalesced per main-loop iteration; the click path and the "pointless drag" branch left picking inline | a hover flood is many events resolving to one visible position, so only the last matters; a click is a single event whose pick must resolve before the same event decides whether to open a context menu, and the drag branch's question ("did the cursor leave the node it was pressed on") is one every intermediate position can answer differently |
| 2026-08-08 | `--record` greys out File → Rescan/Change Root instead of teaching the recording loop to apply them | applying one mid-capture would free the tree the recording script's own cues hold `GNode *` into; a menu item that cannot do its job should not look like it can |
| 2026-08-08 | 3D label glyphs rasterized from a system font at startup rather than baking a wider XBM charset into the repo | a baked atlas would have to be regenerated by hand for every range anyone ever wants, and 336 cells of hand-drawn bitmap is not reviewable; stb_truetype is ~5k lines already vendored for ImGui, and the XBM stays as the zero-dependency fallback |
| 2026-08-08 | NFC normalization cached in `NodeDesc::dname` at scan time, not applied at draw time | the 3D label path runs per visible node per frame; normalizing there would re-shape every string 60×/s. Storing it beside (never *instead of*) the byte-exact `name` keeps every filesystem call, wildcard match and sort untouched |
| 2026-08-08 | Glyph coverage stops at Latin Extended-A (+ dashes/quotes/€) for the 3D atlas | the atlas is a fixed-cell grid sized up front; CJK would need thousands of cells and a proportional-width layout engine. Uncovered codepoints degrade to one `?` each, which is honest and cheap. The ImGui panels have no such limit (1.92 loads glyphs on demand) |
| 2026-08-08 | `lib/stb_truetype.h` is a second, verbatim copy of ImGui's `imstb_truetype.h` rather than an include of it | `src/fontatlas.c` is plain C compiled into *both* frontends, and the GTK arm has no ImGui at all; a copy keeps the two updatable independently and the C arm free of any `subprojects/imgui/` dependency |
| 2026-08-08 | fsn-mode Task A1: sky drawn as `SKY_BANDS` (32) flat-colored horizontal strips instead of a real vertex-colored gradient quad | `FsvVertex`/`gpu_draw()` have no per-vertex color channel — fill color is a uniform (`gpu_set_color()`) — so a true gradient would need a new vertex format, a dedicated pipeline and a new compiled shader pair (MSL+SPIR-V); 32 flat bands is invisible banding in a screenshot at zero shader-toolchain cost |
| 2026-08-08 | fsn-mode Task A1 (superseded same day by the fix round below): sky drawn through a temporary identity projection/modelview at `z=0.999` NDC instead of a depth-write-off pipeline variant | rejected after review: `glm_frustum_rh_zo`'s non-linear zero-to-one depth means a fixed NDC depth near 1.0 still falls within the *linear* world-space depth range real geometry can legitimately occupy near the far clip plane at MapV/TreeV's 128:1 near:far ratio, which would make the sky wrongly occlude that geometry instead of always losing to it — see `FSV_DEPTH_ALWAYS_NOWRITE` below |
| 2026-08-08 | fsn-mode Task A1 fix round: added `FSV_DEPTH_ALWAYS_NOWRITE` (`gpu.h`/`pipeline_for()`) — depth test disabled outright, not left enabled with `SDL_GPU_COMPAREOP_ALWAYS` — for the landscape sky instead of tuning the rejected NDC-depth constant | disabling the test is what SDL_GPU (mirroring Vulkan/Metal) actually ties the "no depth write" guarantee to, on every backend, regardless of the projection's shape; a compare-op tweak on the old approach would still have been exploitable at a big enough near:far ratio |
| 2026-08-08 | fsn-mode Task A1 fix round: ground plane gated to `FSV_MAPV`/`FSV_TREEV` only (`draw_landscape()`'s explicit, `SWITCH_FAIL`-terminated switch on `globals.fsv_mode`), not drawn unconditionally in every mode | confirmed by screenshot that an unconditional ground plane produces a full-frame green wall in `FSV_DISCV` for any realistically-sized directory (camera there looks straight down the world Z axis at a fixed distance — it never reads `phi`/`theta` the way MapV/TreeV do — so the ground quad, sitting just past the disc's own content, fills the entire view once `distance ≥ 96`); a 2-file test fixture's small default distance masked this at first and the earlier claim that "DiscV's ground does not render at all" was wrong |
| 2026-08-08 | fsn-mode Task A1: landscape persistence (`landscape_get/_set/_init`) lives in `src/color.c`/`color.h` rather than a new module | identical shape to the color config already there (nvstore-backed, read once at startup, written immediately on change) — a new file would duplicate the open/close-per-call pattern for no isolation benefit |
| 2026-08-08 | fsn-mode Task B1: `FSV_FSN` inserted into `FsvMode` before `FSV_SPLASH`, not appended after `FSV_NONE` | it lands among the real visualization modes, so every `switch` ending in `SWITCH_FAIL` must account for it; a missed one aborts loudly (`g_assert_not_reached`) instead of silently taking a wrong arm — exactly the failure mode wanted while the mode is being built out |
| 2026-08-08 | fsn-mode Task B1: FSN split across two translation units (`geometry-fsn.c` layout in `libfsvcore`, `geometry-fsn-draw.c` drawing per frontend) rather than one file next to `geometry.c` | the layout is pure math that `camera.c` (itself core) reads, and keeping it gpu-free is the property the headless layout test exists to protect — with the split, `tests/test_fsn_layout.c` links with zero renderer stubs, so the linker enforces the invariant. The single-file arrangement was tried first and broke `fsv-scan`'s link: stubbing `fsn_layout_*` for `libfsvcore`'s consumers would have collided with the test's own compiled-in copy |
| 2026-08-08 | fsn-mode Task B1: FSN geometry stored in `NodeDesc::geomparams`/`DirNodeDesc::geomparams2` like every other mode, rather than a side table | `FsnPedestal` is exactly five doubles, which is `geomparams`'s exact size (MapV plays the same trick), and the three extra per-directory values the two-pass layout carries fit `geomparams2` exactly; a side table would need its own lifetime tied to a tree that `scanfs()` already frees wholesale |
| 2026-08-08 | fsn-mode Task B1: pedestal and file-box heights are `log2` of size, clamped — not linear, not sqrt | a real source tree spans five or six orders of magnitude of subtree size; anything gentler leaves the root pedestal towering over everything else in the same frame, where the reference screenshot's pedestals are all within a small factor of each other |
| 2026-08-08 | fsn-mode Task B1: FSN camera reuses MapV's *storage* (`MAPV_CAMERA`, the Cartesian target) but not MapV's *math* | `mapv_look_at()` and every other `mapv_*` camera helper is written in `MAPV_GEOM_PARAMS`, which holds an `FsnPedestal` in FSN mode (the modes share `NodeDesc::geomparams`) — delegating outright would feed the camera another mode's numbers reinterpreted as its own. The storage, by contrast, is genuinely the same shape, so the pan/morph arms fall through to MapV's |
| 2026-08-08 | fsn-mode Task B1: `geometry.c`'s `node_set_color()` exported as `geometry_node_set_color()` instead of copied into `geometry-fsn-draw.c` | one select-pass id encoding and one highlight boost for both files rather than two that could drift; `highlight_node_id` is `geometry.c`'s private state, so a copy could not have shared it anyway |
| 2026-08-08 | fsn-mode Task B1: FSN wires draw black in the select pass rather than being skipped | same reasoning as `draw_lit()`'s fixed-color path above (2026-08-07): skipping drops them from the pick pass's depth buffer, letting a click pass through to whatever sits behind; id 0 keeps the occlusion honest while correctly reporting "not a node" |
| 2026-08-08 | fsn-mode Task B1 fix round: FSN's ground path text caches its composed string, keyed on the `GNode *` it came from, with invalidation in `fsn_geometry_draw()` rather than `fsn_geometry_free()` | `node_absname_display()`'s own contract (`src/common.c`) is "hover/selection changes, not per node per frame", and this call was per frame; the invalidation cannot live in `fsn_geometry_free()` because that is the layout half, which `libfsvcore` links and the draw file is absent from — a call in that direction would not link. Measured at 3 cache misses over 300 recorded frames with two scripted `camera_look_at_full()` cues |
| 2026-08-08 | fsn-mode Task B1 fix round: `tests/fixture` gained `dir-c` (a second top-level directory) and `dir-a/dir-d` + `dir-a/dir-e` | the FSN layout's ground-width slicing was untestable on the old fixture: with one child, `FSN_DIR_SPAN`'s `max(own width, children's total)` rule has no effect and a broken implementation passes — confirmed by breaking it deliberately before the fixture grew and watching the test still pass. Three siblings under `dir-a` make its subtree several times wider than its own pedestal, which is what makes the rule observable. `test_scanfs`'s bound is a floor (`>= 6`), so it is unaffected by design |
| 2026-08-08 | fsn-mode Task B1 fix round: file-box height left unclamped against its pedestal's height, with the comment corrected instead | box height *is* the file's size; clamping it against the pedestal it happens to stand on would render two identically-sized files at different heights depending on which directory they are in, which misinforms worse than a tall box on a short slab |
| 2026-08-08 | fsn-mode Task B3: selection spotlight drawn as `FSN_SPOTLIGHT_RING_COUNT` (6) stacked flat-alpha ellipses instead of a true per-vertex alpha gradient | same root cause as Task A1's banded sky: `FsvVertex`/`gpu_draw()` carry no per-vertex color, only a uniform fill color; a real gradient would need a new vertex format, pipeline and shader pair for one decorative decal |
| 2026-08-08 | fsn-mode Task B3: added `FSV_DEPTH_LESS_NOWRITE` (`gpu.h`) and tied alpha blending to that one depth-test value in `pipeline_for()`, rather than adding a `gpu_spotlight()` entry point or a separate blend flag on `gpu_draw()` | the spotlight is the only caller that needs depth-test-on/write-off *and* blending together; `gpu_set_depth_test()` already selects `gpu_draw()`'s pipeline per call, so no new gpu.h surface was needed at all — the smaller of the two extensions the task brief offered |
| 2026-08-08 | fsn-mode Task B3: a file's spotlight sits on its *parent's* pedestal top (world `z == parent_ped->h`), a directory's on the true ground (`z == 0`) — both read via `fsn_layout_get()`, no new layout accessor | matches what "the ground under the selected node" actually means physically: a file's local ground is the pedestal it stands on, not the world floor several generations below it |
| 2026-08-08 | fsn-mode Task B3: `fsn_node_visible()`'s ancestor walk starts one generation higher for a file than for a directory | `fsn_draw_recursive()` draws a directory's own box *and own files* unconditionally, before checking its own `collapsed` flag — that flag only gates recursion into *child directories* — so a file's visibility depends on its parent being *reached*, not on the parent's own collapsed state, while a directory's visibility depends on its parent not being collapsed directly. Getting this backwards (checking the immediate parent's collapsed flag for a file too) was the task's own first draft, caught by literally testing "collapse the selected node's parent" for both node kinds rather than trusting the more intuitive-sounding rule |
| 2026-08-08 | fsn-mode Task B3: FSN auto-landscape persists a separate `landscape_explicit` nvstore boolean rather than inferring "explicit" from whether `landscape` differs from the built-in default | the built-in default ("slate") is itself a legitimate explicit choice a user could make from the menu, indistinguishable from "never chosen" by value alone; a dedicated flag is the only way to tell the two apart |
| 2026-08-08 | fsn-mode Task B3: leaving FSN mode restores no prior landscape (no "landscape before FSN" is saved) | keeping the feature to what the brief asked for (auto-*entering* FSN) — a restore-on-exit would need its own saved-state slot and its own interaction with the explicit flag for arguably little benefit, since the landscape menu remains one click away in any mode |
| 2026-08-09 | fsn-mode Task C3: `SDL_OpenURL()` + `g_filename_to_uri()`, never a direct `exec()`/`system()` of the file | delegates the "what happens next" decision to the OS's own default-application resolver (LaunchServices on macOS, `xdg-open` on Linux) — the exact mechanism a Finder double-click already uses — keeping this program's own responsibility limited to handing over a correctly percent-encoded URL |
| 2026-08-09 | fsn-mode Task C3: extended the *existing* `impatient_reclick` exemption (`NODE_IS_DIR`) in `input.cpp`'s `BUTTON_DOWN` case to also cover an FSN-eligible file, rather than leaving the new open-file branch to rely on `camera_moving()` settling on its own | `FSN_CAMERA_MIN_PAN_TIME` (0.5s) routinely outlasts a real double-click's inter-click interval — confirmed by a verification harness whose *first* pass fed a zero-delay double-click and found the modal never opened, because the second click's press was silently discarded by the pre-existing "impatient user" path before this task's own code ever ran |
| 2026-08-09 | fsn-mode Task C3: the confirm modal is a true `BeginPopupModal()`, positioned via explicit `SetNextWindowPos()` off `GetMainViewport()->WorkPos`, not ImGui's own default placement | a true modal makes `io.WantCaptureKeyboard` true for free (verified in `imgui.cpp`), so Escape is handled without touching `ui_dialogs_handle_escape()`; explicit positioning avoids ImGui's default first-use placement landing the modal directly under the cursor that just double-clicked to open it |
| 2026-08-09 | fsn-mode Task C3: `open_files_allowed` is a `ui_dialogs.cpp`-local static with its own `ui_dialogs_init()`, not a new `color.h` accessor alongside `landscape_explicit()` | nothing outside this file — not even the GTK arm, which has no equivalent gesture — ever needs to ask it |
| 2026-08-09 | fsn-mode Task C3 fix round: `g_open_file_request.pending` gets its own same-drain-Esc guard in `input.cpp`, placed immediately after `g_context_menu_request.pending`'s existing one, rather than a generic "any pending request" check | keeps each guard's own comment specific to the request it cancels (matches the file's existing one-guard-per-seam style) and avoids a shared helper for exactly two call sites |
| 2026-08-09 | fsn-mode Task C3 fix round: documented the `g_filename_to_uri()` NULL-path fixture as *empirically unconstructible on macOS/APFS* (kernel-level filename validation rejects the byte sequence outright), rather than mocking the function to force it | a mock would test the mock, not the real code; the actual constraint is the platform's, confirmed by trying it directly (`touch $'bad\xffname.txt'` → "Illegal byte sequence"), not assumed |
| 2026-08-09 | fsn-mode Task C4: FSN's directory double-click never collapses (no `colexp(COLLAPSE)` call anywhere in the branch), rather than tracking "is the camera currently warped into this node" as a flag and gating collapse on it | upstream fsn's warp was never a toggle in the first place; the branch structure itself (auto-expand-if-collapsed, then always `camera_warp_to()`, no collapse arm) makes "re-double-click never collapses" true by construction with no runtime state to get out of sync, the smallest option the task brief itself offered |
| 2026-08-09 | fsn-mode Task C4: factored `camera_look_at_full()`'s prologue/epilogue into `camera_pan_begin()`/`camera_pan_commit()` so `camera_warp_to()` could share them, rather than duplicating ~25 lines (flight-end, access-disable, birdseye-off, scroll-save, pan-break; master-morph-arm, history-push, current-node/manual-control bookkeeping) | the "hook pattern" the task brief asked warp to mirror is exactly this prologue/epilogue; two functions with the same shape and no shared body is the kind of copy this codebase's other tasks (B2's `cancel_pan_for_manual_control()`, C1/C2's accessor-over-copy) have already refactored away rather than repeated |
| 2026-08-09 | fsn-mode Task C4: `camera_warp_to()` takes no `MorphType`/`pan_time_override` (unlike `camera_look_at_full()`) | its one caller (`input.cpp`'s FSN double-click branch) always wants the same `MORPH_SIGMOID` landing; a parameter nothing ever varies is dead surface, and dropping it means `camera.h`'s declaration needs no `#ifdef FSV_ANIMATION_H` guard the way `camera_look_at_full()`'s does |
| 2026-08-09 | fsn-mode Task C4: the warp's look-at target is the pedestal top raised by a fixed `FSN_WARP_HEIGHT_LIFT`, not the bare pedestal surface (`ped->h` alone) | confirmed by screenshot: an unraised target at a low elevation put the camera in the aisle between two rows of file boxes, staring down a canyon of their side walls — the opposite of the "file boxes fill the view" the task asked for; raising the aim point together with the elevation is what clears the camera over the box canopy |
| 2026-08-09 | fsn-mode Task C4: the wrapped-theta short-arc check calls `camera_warp_to()` directly rather than through a simulated double-click | by the time that check runs in the verification script, the camera has already warped in close, so a click at the pre-warp screen position lands on one of the pedestal's own file-box children (the very effect this task built) rather than the pedestal itself — confirmed the hard way when an earlier pass's stray click there opened Task C3's file-open confirm modal and silently blocked every later synthetic click in the same script via `io.WantCaptureMouse` |
