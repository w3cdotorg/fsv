# Porting fsv to macOS / Metal

This branch (`metal-port`) replaces the GTK3 + OpenGL frontend with
SDL3 + SDL_GPU (Metal on macOS) + Dear ImGui.

- Plan: [docs/superpowers/plans/2026-08-06-macos-metal-port.md](superpowers/plans/2026-08-06-macos-metal-port.md)
- Upstream: https://github.com/jabl/fsv (tracked on `master`)
- Status: **M1 (headless core) done — M2 (dependencies + app skeleton) done — M3 in progress through Task 3.2**

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
