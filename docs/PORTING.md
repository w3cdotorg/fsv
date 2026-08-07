# Porting fsv to macOS / Metal

This branch (`metal-port`) replaces the GTK3 + OpenGL frontend with
SDL3 + SDL_GPU (Metal on macOS) + Dear ImGui.

- Plan: [docs/superpowers/plans/2026-08-06-macos-metal-port.md](superpowers/plans/2026-08-06-macos-metal-port.md)
- Upstream: https://github.com/jabl/fsv (tracked on `master`)
- Status: **M1 (headless core) done — M2 (dependencies + app skeleton) done — M3 done — M4 done through Task 4.1**

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
| Double-click | Explicit no-op (`GDK_2BUTTON_PRESS: break;`) | No branch on `ev->button.clicks` | Reproduces the no-op by construction — SDL has no extra event to ignore |
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
