# fsv macOS/Metal Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port fsv (3D file system visualizer, descendant of SGI's fsn) to macOS with a native Metal rendering path, by replacing the GTK3 + GtkGLArea + OpenGL frontend with SDL3 + SDL_GPU (Metal backend) + Dear ImGui, while reusing the platform-independent core (~70% of the codebase).

**Architecture:** Strangler-style port on branch `metal-port`. The core logic (`scanfs.c`, `geometry.c` scene graph, `camera.c` math, `colexp.c`, `color.c`, `common.c`, `animation.c`) keeps its C/GLib implementation and is decoupled from the frontend through a small platform-hooks header. A new C++20 frontend under `src/sdl/` provides the window, main loop, input, SDL_GPU renderer, and ImGui-based UI. GTK files (`gui.c`, `dialog.c`, `window.c`, `callbacks.c`, `dirtree.c`, `filelist.c`, `viewport.c`, `ogl.c`, `about.c`, `tmaptext.c`) are ported or replaced, never patched in place.

**Tech Stack:** C (core) + C++20 (frontend) · GLib (data structures only — GNode, GList; **not** GTK) · SDL3 ≥ 3.2 with SDL_GPU API (Metal on macOS, Vulkan on Linux) · Dear ImGui ≥ 1.91.6 (SDL3 + SDLGPU3 backends, vendored) · cglm (already used upstream) · Meson/ninja · shaders authored in Vulkan-flavored GLSL 4.50, precompiled to MSL + SPIR-V, compiled artifacts checked in.

## Global Constraints

- Branch: all work on `metal-port`; `master` stays clean to track `upstream` (https://github.com/jabl/fsv).
- Commit after every green step; conventional-commit prefixes (`feat:`, `refactor:`, `build:`, `docs:`, `ci:`).
- All documentation in **English**.
- New frontend files live in `src/sdl/`; core C files stay in `src/` and must keep compiling **without** any GTK or epoxy include.
- Core stays C11; new files are C++20. Core headers included from C++ must be wrapped in `extern "C"` (add guards inside the headers, not at include sites).
- macOS floor: macOS 14 (Sonoma), Apple Silicon + Intel. Linux (Vulkan) must keep compiling but is best-effort until M6.
- GLib remains a dependency (brew `glib`); GTK3/epoxy dependencies must be **gone** from the SDL frontend build by end of M2.
- No OpenGL calls anywhere in the SDL frontend — SDL_GPU only. `#include <epoxy/gl.h>` is forbidden in `src/sdl/`.
- Each milestone ends with a buildable, runnable deliverable. Do not start milestone N+1 before N is approved (per project CLAUDE.md phased-execution rule).
- Meson option `-Dfrontend=sdl|gtk` selects the frontend; `gtk` remains the default until M5 completes, then `sdl` becomes default on macOS.

## Current-State Map (verified 2026-08-06 against jabl/fsv @ master)

| File | LOC | GTK symbols | GL calls | Fate |
|---|---|---|---|---|
| `src/geometry.c` | 3339 | 0 | 136 | Keep; GL calls migrate behind `src/sdl/gpu.h` |
| `src/dialog.c` | 1441 | heavy | 0 | Replace with ImGui (`src/sdl/ui_dialogs.cpp`) |
| `src/gui.c` | 1423 | heavy | 0 | Replace (`src/sdl/ui_main.cpp`) |
| `src/camera.c` | 1402 | 71 (scrollbars only) | 0 | Keep; scrollbar coupling → hooks |
| `src/common.c` | 888 | 0 | 0 | Keep as-is |
| `src/color.c` | 592 | 0 | 0 | Keep as-is |
| `src/ogl.c` | 547 | some | 91 | Port to `src/sdl/gpu.cpp` (matrices, picking) |
| `src/tmaptext.c` | 518 | 0 | 55 | Port to `src/sdl/text3d.cpp` |
| `src/filelist.c` | 466 | heavy | 0 | Replace (`src/sdl/ui_panels.cpp`) |
| `src/animation.c` | 447 | 0 (GLib g_idle only) | 0 | Keep; loop scheduling → hooks |
| `src/scanfs.c` | 370 | 0 | 0 | Keep as-is |
| `src/dirtree.c` | 355 | heavy | 0 | Replace (`src/sdl/ui_panels.cpp`) |
| `src/window.c` | 330 | heavy | 0 | Replace (`src/sdl/main.cpp`) |
| `src/colexp.c` | 313 | 0 | 0 | Keep as-is |
| `src/viewport.c` | ~300 | heavy | few | Replace (`src/sdl/input.cpp`) |
| `src/about.c` | ~200 | some | 5 | Drop initially; ImGui About window in M5 |

Picking upstream is already modern GL: color-ID offscreen render + `glReadPixels` (`ogl_select_modern()` in `src/ogl.c:456`). This maps 1:1 to an SDL_GPU render target + texture download.

The animation loop is GLib-idle driven (`src/animation.c:440`, `g_idle_add_full` → `animation_loop()` → `ogl_draw()`); in the SDL frontend the main loop drives frames, so the loop body must be callable as a per-frame tick.

## Target File Structure

```
src/
  fsv-platform.h        NEW  hooks connecting core → frontend (C, no deps)
  scanfs.c colexp.c color.c common.c animation.c camera.c geometry.c fsv.c   KEEP (core)
  gui.c dialog.c window.c callbacks.c dirtree.c filelist.c viewport.c ogl.c  GTK frontend (untouched, still builds with -Dfrontend=gtk)
  sdl/
    main.cpp            NEW  entry point, SDL init, main loop, frame pacing
    gpu.h / gpu.cpp     NEW  SDL_GPU device, pipelines, mesh API, matrices, picking
    input.cpp           NEW  mouse/keyboard → camera & selection (port of viewport.c)
    text3d.cpp          NEW  3D text labels (port of tmaptext.c)
    ui_main.cpp         NEW  menu bar, layout, mode switching (replaces gui.c/window.c)
    ui_panels.cpp       NEW  dir tree + file list panels (replaces dirtree.c/filelist.c)
    ui_dialogs.cpp      NEW  color setup, properties, about (replaces dialog.c/about.c)
shaders/
  src/*.vert *.frag     NEW  GLSL 4.50 sources (ported from src/fsv-*.glsl)
  compiled/*.msl *.spv  NEW  checked-in artifacts
tools/
  fsv-scan.c            NEW  headless core smoke-test CLI
  compile-shaders.sh    NEW  regenerates shaders/compiled/ via SDL_shadercross
subprojects/
  imgui/                NEW  vendored Dear ImGui (docking branch) + SDL3/SDLGPU3 backends
tests/
  fixture/              NEW  small known directory tree for scan tests
  test_scanfs.c         NEW  meson-registered unit test
docs/
  PORTING.md            NEW  running notes, decisions, deviations from this plan
```

---

## Milestone 0 — Repo bootstrap (branch, docs)

### Task 0.1: Branch, plan, porting notes, README banner

**Files:**
- Create: `docs/superpowers/plans/2026-08-06-macos-metal-port.md` (this file)
- Create: `docs/PORTING.md`
- Modify: `README.md` (top banner only)

- [x] **Step 1: Create branch and commit the plan**

```bash
git checkout -b metal-port   # if not already on it
git add docs/superpowers/plans/2026-08-06-macos-metal-port.md
git commit -m "docs: add macOS/Metal port implementation plan"
```

- [x] **Step 2: Write docs/PORTING.md**

```markdown
# Porting fsv to macOS / Metal

This branch (`metal-port`) replaces the GTK3 + OpenGL frontend with
SDL3 + SDL_GPU (Metal on macOS) + Dear ImGui.

- Plan: docs/superpowers/plans/2026-08-06-macos-metal-port.md
- Upstream: https://github.com/jabl/fsv (tracked on `master`)
- Status: M0 in progress

## Decision log
| Date | Decision | Why |
|---|---|---|
| 2026-08-06 | SDL_GPU over raw Metal | C API, cross-platform, Metal backend native |
| 2026-08-06 | Check in compiled shaders (MSL+SPIR-V) | avoids SDL_shadercross as a build dep |
| 2026-08-06 | Keep GLib, drop GTK | core relies on GNode/GList; GLib is headless-safe |
```

- [x] **Step 3: Add README banner under the title**

```markdown
> **⚠️ metal-port branch** — this fork is porting fsv to macOS with a native
> Metal renderer (SDL3 GPU + Dear ImGui). See `docs/PORTING.md`.
> For the stable GTK/OpenGL version, use [jabl/fsv](https://github.com/jabl/fsv).
```

- [x] **Step 4: Commit and push**

```bash
git add docs/PORTING.md README.md
git commit -m "docs: add porting notes and README banner"
git push -u origin metal-port
```

---

## Milestone 1 — Core builds headless on macOS (no GTK)

Deliverable: `fsvcore` static library + `fsv-scan` CLI compile and pass a unit test on macOS with only brew `glib`, `cglm`, `meson`.

### Task 1.1: Platform hooks header

**Files:**
- Create: `src/fsv-platform.h`
- Modify: `src/animation.c` (replace `g_idle_add_full`/`ogl_draw` coupling)
- Test: compile-only (wired in Task 1.3)

**Interfaces:**
- Produces: `FsvPlatformHooks` struct and `fsv_platform` global consumed by every later task:

```c
/* src/fsv-platform.h — SPDX-License-Identifier: MIT */
#ifndef FSV_PLATFORM_H
#define FSV_PLATFORM_H

#ifdef __cplusplus
extern "C" {
#endif

/* Frontend services the core may request. Every field must be non-NULL
 * after frontend init (the GTK frontend and the SDL frontend each fill
 * this in before fsv core code runs). */
typedef struct {
	/* Ask the frontend to schedule (at least) one more frame.
	 * GTK impl: g_idle_add of animation tick. SDL impl: set a
	 * "frame requested" flag read by the main loop. */
	void (*request_frame)(void);
	/* Render the 3D viewport now (called from the animation tick). */
	void (*render_frame)(void);
	/* Viewport size in pixels (needed by camera + picking). */
	void (*viewport_size)(int *width, int *height);
	/* Scroll state for MapV/TreeV camera panning; replaces direct
	 * GtkAdjustment access in camera.c. `pos` in [lower, upper-page]. */
	void (*set_scroll)(int axis /*0=x,1=y*/, double lower, double upper,
	                   double page, double pos);
	double (*get_scroll)(int axis);
} FsvPlatformHooks;

extern FsvPlatformHooks fsv_platform;

/* One iteration of the animation/morph loop. Returns non-zero while
 * animation is still active (frontend should keep scheduling frames). */
int fsv_animation_tick(void);

#ifdef __cplusplus
}
#endif
#endif
```

- [x] **Step 1: Add the header exactly as above; define the global in `animation.c`**

```c
/* in src/animation.c, near top */
#include "fsv-platform.h"
FsvPlatformHooks fsv_platform; /* zero-initialized; frontends fill it in */
```

- [x] **Step 2: Refactor the animation loop**

In `src/animation.c`, rename the static `animation_loop()` body into the public tick and re-express both entry points through it:

```c
int
fsv_animation_tick(void)
{
	boolean state_changed, schevents_pending = FALSE;

	state_changed = morph_iteration( );

	if (globals.need_redraw) {
		fsv_platform.render_frame( );          /* was: ogl_draw( ) */
		framerate_iteration( FRAME_RENDERED );
		schevents_pending = scheduled_event_iteration( );
		if (!schevents_pending)
			globals.need_redraw = FALSE;
	}

	if (!state_changed && !schevents_pending) {
		framerate_iteration( STOP_TIMING );
		animation_active = FALSE;
	}
	return animation_active;
}

void
redraw( void )
{
	if (!animation_active)
		fsv_platform.request_frame( );   /* was: g_idle_add_full(...) */
	animation_active = TRUE;
	globals.need_redraw = TRUE;
}
```

Keep the GTK frontend working: in `src/gui.c` (or `window.c` init path), install hooks that reproduce the old behavior:

```c
static gboolean gtk_tick_cb(gpointer data) { return fsv_animation_tick(); }
static void gtk_request_frame(void)
{ g_idle_add_full(G_PRIORITY_LOW, gtk_tick_cb, NULL, NULL); }
static void gtk_render_frame(void) { ogl_draw(); }
/* ...assign into fsv_platform during gui init, alongside existing
   viewport-size and GtkAdjustment-backed scroll implementations. */
```

- [x] **Step 3: Build the GTK frontend on Linux CI or skip if no Linux at hand — at minimum `meson setup builddir && ninja -C builddir` must still succeed in a Linux container. Record outcome in docs/PORTING.md.**

- [x] **Step 4: Commit**

```bash
git add src/fsv-platform.h src/animation.c src/gui.c
git commit -m "refactor: decouple animation loop from GTK/GL via platform hooks"
```

### Task 1.2: Decouple camera.c from GtkAdjustment

**Files:**
- Modify: `src/camera.c` (71 GTK symbol uses, all scrollbar-related)
- Modify: `src/camera.h` (drop `camera_pass_scrollbar_widgets`, add hook-based equivalent)
- Modify: `src/gui.c` (provide GtkAdjustment-backed `set_scroll`/`get_scroll`)

**Interfaces:**
- Consumes: `fsv_platform.set_scroll` / `fsv_platform.get_scroll` from Task 1.1.
- Produces: `camera.h` API unchanged except `camera_pass_scrollbar_widgets(GtkWidget*, GtkWidget*)` is deleted; all internal `gtk_adjustment_*` reads/writes go through the two hooks.

- [x] **Step 1: Replace every `gtk_adjustment_get_value(adj) ...` pattern in camera.c.** Example of the transformation (from `src/camera.c:304`):

```c
/* before */
value = gtk_adjustment_get_value(adj) + 0.5 * gtk_adjustment_get_page_size(adj);
/* after — page size becomes a camera-side computed value passed on set */
value = fsv_platform.get_scroll(axis) + 0.5 * page_size;
```

Track `lower/upper/page` in a small static struct per axis inside camera.c (they are computed there anyway before being pushed into the adjustments today).

- [x] **Step 2: Move the GtkAdjustment plumbing into gui.c** behind `set_scroll`/`get_scroll` so `-Dfrontend=gtk` behaves identically.

- [x] **Step 3: Verify camera.c no longer includes gtk**

Run: `grep -n "gtk\|Gtk\|GTK" src/camera.c`
Expected: no matches (remove the `#include <gtk/gtk.h>` at `src/camera.c:16`).

- [x] **Step 4: Commit**

```bash
git add src/camera.c src/camera.h src/gui.c
git commit -m "refactor: route camera scroll state through platform hooks, drop GTK from camera.c"
```

### Task 1.3: fsvcore static lib + fsv-scan smoke CLI + unit test

**Files:**
- Modify: `src/meson.build` (define `libfsvcore`)
- Create: `tools/fsv-scan.c`
- Create: `tests/fixture/` (committed small tree), `tests/test_scanfs.c`
- Modify: root `meson.build` (add `frontend` option handling, tests)
- Create: `meson_options.txt` entry

**Interfaces:**
- Produces: meson target `libfsvcore` = `[scanfs.c, colexp.c, color.c, common.c, animation.c, camera.c]` + deps `[glib-2.0, cglm, libm]`. (geometry.c joins the lib in M3 once its GL calls are gone.)

- [x] **Step 1: meson_options.txt**

```meson
option('frontend', type: 'combo', choices: ['gtk', 'sdl'], value: 'gtk',
       description: 'UI frontend: gtk (legacy OpenGL) or sdl (SDL3 GPU / Metal)')
```

- [x] **Step 2: Define the core lib in src/meson.build**

```meson
glibdep = dependency('glib-2.0')
fsvcore_src = files('scanfs.c', 'colexp.c', 'color.c', 'common.c',
                    'animation.c', 'camera.c')
libfsvcore = static_library('fsvcore', fsvcore_src,
  dependencies: [glibdep, cglm_dep, libm],
  include_directories: '.')
```

Note: `animation.c`/`camera.c` may still include `"ogl.h"` for constants — remove those includes; anything still needed moves to `fsv.h` or `fsv-platform.h`. `camera.c` calls into `geometry.c`/`gui.c` symbols; for the core lib to link into `fsv-scan`, keep `fsv-scan` linking only against the objects it needs, or add `-Wl,-undefined,dynamic_lookup`-free stubs: simplest correct approach is to give `fsv-scan` its own link line with `scanfs.c colexp.c color.c common.c` objects only (meson `extract_objects`), and leave full-lib linking to the real frontends. Choose that if the full lib does not link standalone, and note it in PORTING.md.

- [x] **Step 3: Write tools/fsv-scan.c**

```c
/* fsv-scan: headless smoke test for the fsv core scanner.
 * Usage: fsv-scan <directory> */
#include <stdio.h>
#include <glib.h>
#include "fsv.h"
#include "scanfs.h"
#include "common.h"

int
main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <directory>\n", argv[0]);
		return 2;
	}
	scanfs(argv[1]);   /* exact entry point: see scanfs.h; adjust if
	                      signature differs (it takes the root path
	                      and populates globals.fstree) */
	GNode *root = globals.fstree;
	if (root == NULL) {
		fprintf(stderr, "scan produced no tree\n");
		return 1;
	}
	printf("nodes=%u\n", g_node_n_nodes(root, G_TRAVERSE_ALL));
	return 0;
}
```

(The scanner's real signature and the tree global must be read from `src/scanfs.h` / `src/fsv.h` at implementation time and the CLI adjusted; the test contract below is what's fixed.)

- [x] **Step 4: Create the fixture and the test**

```bash
mkdir -p tests/fixture/dir-a/dir-b
printf 'hello' > tests/fixture/file1.txt          # 5 bytes
printf '12345678' > tests/fixture/dir-a/file2.bin # 8 bytes
printf 'x' > tests/fixture/dir-a/dir-b/file3      # 1 byte
```

```c
/* tests/test_scanfs.c — asserts the scanner sees the fixture correctly */
#include <assert.h>
#include <glib.h>
#include "fsv.h"
#include "scanfs.h"

int
main(void)
{
	scanfs(FIXTURE_DIR);  /* FIXTURE_DIR injected by meson */
	assert(globals.fstree != NULL);
	/* 3 files + 3 dirs (fixture root, dir-a, dir-b) */
	assert(g_node_n_nodes(globals.fstree, G_TRAVERSE_ALL) >= 6);
	return 0;
}
```

```meson
# in root meson.build (or tests/meson.build)
test_scanfs = executable('test_scanfs', 'tests/test_scanfs.c',
  objects: libfsvcore.extract_objects('scanfs.c', 'colexp.c', 'color.c', 'common.c'),
  dependencies: [glibdep, cglm_dep, libm],
  include_directories: 'src',
  c_args: ['-DFIXTURE_DIR="@0@"'.format(meson.current_source_dir() / 'tests/fixture')])
test('scanfs', test_scanfs)
```

- [x] **Step 5: Run the test on macOS, verify it fails before wiring, passes after**

```bash
brew install glib cglm meson ninja pkgconf
meson setup builddir -Dfrontend=gtk   # gtk frontend won't build on mac; core targets must
ninja -C builddir test_scanfs fsv-scan && meson test -C builddir scanfs
```

Expected: `1/1 scanfs OK`. If meson refuses to configure because GTK is missing, gate the GTK frontend targets behind `if frontend == 'gtk' and gtkdep.found()` so core targets configure everywhere.

- [x] **Step 6: Commit**

```bash
git add meson_options.txt meson.build src/meson.build tools/fsv-scan.c tests/
git commit -m "build: extract libfsvcore, add headless scan CLI and first unit test"
```

---

## Milestone 2 — SDL3 app skeleton with ImGui (window, Metal clear, main loop)

Deliverable: `fsv -Dfrontend=sdl` opens a native macOS window, clears via Metal, shows the ImGui demo window, drives `fsv_animation_tick()` per frame, quits cleanly.

### Task 2.1: Dependencies (SDL3, vendored ImGui)

**Files:**
- Create: `subprojects/imgui/` (vendored: `imgui*.cpp/h`, `backends/imgui_impl_sdl3.*`, `backends/imgui_impl_sdlgpu3.*`, plus a hand-written `meson.build`)
- Modify: root `meson.build`

- [x] **Step 1: Install SDL3**: `brew install sdl3`. Verify: `pkg-config --modversion sdl3` ≥ 3.2.
- [x] **Step 2: Vendor Dear ImGui (docking branch)** — copy the 9 core files + the two backends into `subprojects/imgui/`, pin the version in `subprojects/imgui/VERSION.txt`, and add:

```meson
# subprojects/imgui/meson.build
project('imgui', 'cpp', default_options: ['cpp_std=c++20'])
sdl3dep = dependency('sdl3')
imgui_lib = static_library('imgui',
  ['imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp',
   'backends/imgui_impl_sdl3.cpp', 'backends/imgui_impl_sdlgpu3.cpp'],
  include_directories: ['.', 'backends'],
  dependencies: sdl3dep)
imgui_dep = declare_dependency(link_with: imgui_lib,
  include_directories: ['.', 'backends'])
```

- [x] **Step 3: Commit** — `git commit -m "build: vendor Dear ImGui with SDL3/SDLGPU3 backends"`

### Task 2.2: main.cpp — window, GPU device, main loop, animation tick

**Files:**
- Create: `src/sdl/main.cpp`
- Create: `src/sdl/meson.build`
- Modify: root `meson.build` (wire `frontend=sdl`)

**Interfaces:**
- Consumes: `fsv_platform`, `fsv_animation_tick()` (Task 1.1).
- Produces: `bool app_frame_requested` semantics; `gpu_init(SDL_Window*)`, `gpu_begin_frame()`, `gpu_end_frame()` are stubbed here and implemented for real in M3 (`src/sdl/gpu.h`).

- [x] **Step 1: Write the skeleton (this is real, compilable SDL3 + ImGui SDLGPU3 wiring — cross-check identifiers against the vendored ImGui example `example_sdl3_sdlgpu3` and adjust if the pinned version drifted):**

```cpp
// src/sdl/main.cpp — SPDX-License-Identifier: MIT
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>
extern "C" {
#include "fsv-platform.h"
}

static bool g_frame_requested = true;   // render first frame
static SDL_Window *g_window = nullptr;
static SDL_GPUDevice *g_device = nullptr;

static void sdl_request_frame(void) { g_frame_requested = true; }
static void sdl_render_frame(void)  { /* scene render lands here in M3 */ }
static void sdl_viewport_size(int *w, int *h) { SDL_GetWindowSizeInPixels(g_window, w, h); }
static double g_scroll[2];
static void sdl_set_scroll(int axis, double, double, double, double pos) { g_scroll[axis] = pos; }
static double sdl_get_scroll(int axis) { return g_scroll[axis]; }

int main(int argc, char **argv)
{
	if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
	g_device = SDL_CreateGPUDevice(
	    SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_SPIRV,
	    /*debug*/ true, nullptr);
	if (!g_device) { SDL_Log("no GPU device: %s", SDL_GetError()); return 1; }
	SDL_Log("GPU driver: %s", SDL_GetGPUDeviceDriver(g_device)); // expect "metal"
	g_window = SDL_CreateWindow("fsv", 1280, 800,
	    SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	SDL_ClaimWindowForGPUDevice(g_device, g_window);

	fsv_platform = { sdl_request_frame, sdl_render_frame, sdl_viewport_size,
	                 sdl_set_scroll, sdl_get_scroll };

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui_ImplSDL3_InitForSDLGPU(g_window);
	ImGui_ImplSDLGPU3_InitInfo ii = {};
	ii.Device = g_device;
	ii.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(g_device, g_window);
	ii.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
	ImGui_ImplSDLGPU3_Init(&ii);

	bool running = true;
	while (running) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			ImGui_ImplSDL3_ProcessEvent(&ev);
			if (ev.type == SDL_EVENT_QUIT) running = false;
		}
		bool animating = fsv_animation_tick() != 0;

		ImGui_ImplSDLGPU3_NewFrame();
		ImGui_ImplSDL3_NewFrame();
		ImGui::NewFrame();
		ImGui::ShowDemoWindow();
		ImGui::Render();

		SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g_device);
		SDL_GPUTexture *swap = nullptr;
		SDL_WaitAndAcquireGPUSwapchainTexture(cmd, g_window, &swap, nullptr, nullptr);
		if (swap) {
			ImDrawData *dd = ImGui::GetDrawData();
			ImGui_ImplSDLGPU3_PrepareDrawData(dd, cmd);
			SDL_GPUColorTargetInfo ct = {};
			ct.texture = swap;
			ct.clear_color = { 0.08f, 0.10f, 0.12f, 1.0f };
			ct.load_op = SDL_GPU_LOADOP_CLEAR;
			ct.store_op = SDL_GPU_STOREOP_STORE;
			SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
			ImGui_ImplSDLGPU3_RenderDrawData(dd, cmd, rp);
			SDL_EndGPURenderPass(rp);
		}
		SDL_SubmitGPUCommandBuffer(cmd);
		if (!animating && !g_frame_requested) SDL_WaitEventTimeout(nullptr, 16);
		g_frame_requested = false;
	}
	// shutdown: ImGui_ImplSDLGPU3_Shutdown, ImGui_ImplSDL3_Shutdown,
	// ImGui::DestroyContext, SDL_ReleaseWindowFromGPUDevice, destroy window/device
	return 0;
}
```

- [x] **Step 2: Wire meson**

```meson
# src/sdl/meson.build
sdl3dep = dependency('sdl3')
imgui_dep = subproject('imgui').get_variable('imgui_dep')
fsv_sdl = executable('fsv', ['main.cpp'],
  link_with: libfsvcore,
  dependencies: [sdl3dep, imgui_dep, glibdep, cglm_dep, libm],
  include_directories: '..',
  install: true)
```

Root meson.build: `if get_option('frontend') == 'sdl'` → `subdir('src/sdl')`, and only look up GTK deps in the `gtk` arm.

- [x] **Step 3: Run and verify**

```bash
meson setup build-sdl -Dfrontend=sdl && ninja -C build-sdl
./build-sdl/src/sdl/fsv
```

Expected: window opens, log line `GPU driver: metal`, ImGui demo renders, Cmd+Q quits, idle CPU near 0% (event-wait path).

- [x] **Step 4: Commit** — `git commit -m "feat: SDL3+Metal app skeleton with ImGui and core animation tick"`

---

## Milestone 3 — Scene rendering on SDL_GPU (the heart of the port)

Deliverable: the three fsv modes (DiscV, MapV, TreeV) render via Metal, camera animation works, `--screenshot out.bmp` produces a non-empty capture for CI.

### Task 3.1: Shader port + offline compilation

**Files:**
- Create: `shaders/src/scene.vert`, `shaders/src/scene.frag`, `shaders/src/text.vert`, `shaders/src/text.frag` (ported from `src/fsv-vertex.glsl`, `src/fsv-fragment.glsl`, `src/fsv-text-*.glsl`)
- Create: `tools/compile-shaders.sh`
- Create: `shaders/compiled/*.msl`, `*.spv` (generated, committed)

- [x] **Step 1: Port each `#version 140` shader to Vulkan-GLSL 4.50.** Mechanical transformation, e.g. for the scene vertex shader:

```glsl
#version 450
layout(set = 1, binding = 0) uniform UBO {
    mat4 mvp;
    mat4 modelview;
    mat3 normal_matrix;   // std140: pad rows to vec4
} u;
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec4 color;
layout(location = 0) out vec4 v_color;
void main() {
    gl_Position = u.mvp * vec4(position, 1.0);
    /* lighting math copied verbatim from src/fsv-vertex.glsl */
    v_color = color;
}
```

Uniform values that upstream sets via `glUniform*` in `ogl.c` move into this single UBO pushed per-frame with `SDL_PushGPUVertexUniformData(cmd, 0, &ubo, sizeof ubo)` (SDL_GPU set/binding conventions: vertex uniforms are set 1, fragment uniforms set 3, fragment samplers set 2).

- [x] **Step 2: tools/compile-shaders.sh**

```bash
#!/usr/bin/env bash
# Regenerates shaders/compiled/. Requires SDL_shadercross (shadercross CLI).
set -euo pipefail
cd "$(dirname "$0")/../shaders"
for f in src/*.vert src/*.frag; do
  base=$(basename "$f"); stage=${base##*.}; name=${base%.*}
  shadercross "$f" -o "compiled/$name.$stage.msl"
  shadercross "$f" -o "compiled/$name.$stage.spv"
done
```

- [x] **Step 3: Run it, commit sources AND artifacts** — `git commit -m "feat: port shaders to GLSL450, add compiled MSL/SPIR-V artifacts"`

### Task 3.2: gpu.h/gpu.cpp — device, pipelines, mesh API, matrices

**Files:**
- Create: `src/sdl/gpu.h`, `src/sdl/gpu.cpp`
- Modify: `src/sdl/main.cpp` (replace stubs)

**Interfaces:**
- Produces (consumed by geometry port, text3d, picking):

```c
/* src/sdl/gpu.h — C linkage so geometry.c (C) can call it */
#ifdef __cplusplus
extern "C" {
#endif
typedef struct FsvMesh FsvMesh;   /* opaque: GPU vertex+index buffers */

typedef struct { float pos[3]; float normal[3]; float color[4]; } FsvVertex;

FsvMesh *fsv_mesh_new(void);
void fsv_mesh_free(FsvMesh *m);
/* Replaces glBufferData on GL_ARRAY_BUFFER/GL_ELEMENT_ARRAY_BUFFER */
void fsv_mesh_upload(FsvMesh *m, const FsvVertex *verts, int nverts,
                     const unsigned int *indices, int nindices);
/* Replaces glDrawElements(GL_TRIANGLES, ...) inside the active pass */
void fsv_mesh_draw(FsvMesh *m);

void gpu_init(void *sdl_window);
void gpu_shutdown(void);
/* Per-frame scene pass; ImGui pass stays in main.cpp */
void gpu_scene_begin(void);   /* begin render pass, bind scene pipeline,
                                 push camera UBO (proj/modelview from cglm,
                                 ported from ogl.c setup_*_matrix()) */
void gpu_scene_end(void);
/* Color-ID picking: render geometry with id-colors offscreen, read 1px.
   Port of ogl_select_modern() (src/ogl.c:456). Returns node id or 0. */
unsigned int gpu_pick(int x, int y);
#ifdef __cplusplus
}
#endif
```

- [x] **Step 1: Implement device/pipeline creation.** Load `shaders/compiled/scene.vert.msl` (or `.spv` when the driver reports SPIRV) with `SDL_CreateGPUShader`; create the scene pipeline with `SDL_CreateGPUGraphicsPipeline` — vertex layout = `FsvVertex` (3 attributes, one buffer, 40-byte stride), depth-stencil target `SDL_GPU_TEXTUREFORMAT_D24_UNORM` with depth test/write on, cull back faces, and a second pipeline variant for picking (no blending, flat id color from a per-draw uniform).
- [x] **Step 2: Implement `FsvMesh`** with `SDL_CreateGPUBuffer` (VERTEX/INDEX usage), uploads through a transfer buffer + copy pass (`SDL_BeginGPUCopyPass`/`SDL_UploadToGPUBuffer`). Meshes are rebuilt on filesystem rescan and camera-independent, so upload once, draw many.
- [x] **Step 3: Port matrix setup** — move `setup_projection_matrix()` / `setup_modelview_matrix()` / `ogl_upload_matrices()` logic from `src/ogl.c` into `gpu.cpp` verbatim (they already use cglm), feeding the UBO instead of `glUniformMatrix4fv`.
- [x] **Step 4: Compile check + commit** — `git commit -m "feat: SDL_GPU renderer core (pipelines, mesh API, camera matrices)"`

**Deviation (Task 3.3):** the depth format actually in use is `D32_FLOAT`,
not `D24_UNORM` — `SDL_GPUTextureSupportsFormat()` reports `D24_UNORM`
unsupported on Apple Silicon, so the code falls back per its own checked
query. The `FsvMesh` retained-handle API sketched here was replaced in
Task 3.3 by an immediate-mode `gpu_draw()` that records-then-replays each
frame (see PORTING.md, Task 3.3 — `geometry.c` owns no persistent meshes,
and SDL_GPU forbids buffer copies inside a render pass, so a per-call-site
mesh reused across nodes painted every node with the last one's geometry).

### Task 3.3: Port geometry.c off OpenGL

**Files:**
- Modify: `src/geometry.c` (136 GL call sites → `gpu.h` API)
- Modify: `src/meson.build` (geometry.c joins libfsvcore — with gpu.h implemented by each frontend: SDL frontend implements it in gpu.cpp; GTK frontend gets a thin `src/ogl-gpu-compat.c` implementing the same 8 functions over epoxy so `-Dfrontend=gtk` still works)

Mapping table to apply mechanically:

| OpenGL in geometry.c | Replacement |
|---|---|
| `glGenBuffers`/`glBindBuffer`/`glBufferData` triples | one `fsv_mesh_upload()` per node/mesh build |
| `glVertexAttribPointer`/`glEnableVertexAttribArray` | gone — fixed `FsvVertex` layout in the pipeline |
| `glDrawArrays`/`glDrawElements` | `fsv_mesh_draw()` |
| `glUniform*` color/id pushes | per-draw uniform via `fsv_mesh_draw` variant `fsv_mesh_draw_id(m, id)` (add to gpu.h if geometry.c needs it for picking colors) |
| `glEnable/glDisable(GL_...)` state toggles | pipeline state — delete; encode in the two pipelines |

- [x] **Step 1: Do the mechanical port file-section by file-section (DiscV, MapV, TreeV builders), compiling after each section.**
- [x] **Step 2: Verify** `grep -c "\bgl[A-Z]" src/geometry.c` → `0`, and no `epoxy` include.
- [x] **Step 3: Wire `sdl_render_frame()`** in main.cpp: `gpu_scene_begin(); geometry_draw(TRUE); gpu_scene_end();` then the existing ImGui pass renders on top in the same swapchain texture (load_op LOAD for the ImGui pass).
- [x] **Step 4: Run `./fsv ~/some/dir`** — expect the classic fsv landscape rendered by Metal; middle-drag not yet wired, camera intro animation (splash → mode) should play since animation ticks are live.
- [x] **Step 5: Add `--screenshot out.bmp` flag** using `SDL_DownloadFromGPUTexture` on the swapchain-sized offscreen target, for CI smoke tests. Verify file is non-black.
- [x] **Step 6: Commit** — `git commit -m "feat: render fsv scene via SDL_GPU/Metal"` (commit per section in Step 1 too).

### Task 3.4: Port tmaptext.c → text3d.cpp (3D name labels)

**Files:**
- Create: `src/sdl/text3d.cpp` (port of `src/tmaptext.c`, 55 GL calls)
- Modify: `src/sdl/gpu.cpp` (text pipeline: alpha-blended, textured quads, `text.vert/frag` shaders)

- [x] **Step 1: Keep the existing font-atlas generation logic from tmaptext.c; swap texture upload to `SDL_CreateGPUTexture` + copy pass; label quads become a dynamic `FsvMesh` rebuilt when labels change.**
- [x] **Step 2: Verify labels render in TreeV mode (directory names on pedestals). Commit** — `git commit -m "feat: port 3D text labels to SDL_GPU"`.

**Deviation:** no separate `src/sdl/text3d.cpp` file — `tmaptext.c`'s GL
surface was small (55 calls: one texture, one program, one draw) against
a lot of shared glyph-layout math, so it was ported in place behind six
new `gpu.h` entry points instead, avoiding either duplicating that math
in a new file or `#include`-ing the original (see PORTING.md, Task 3.4).

---

## Milestone 4 — Interaction (navigate, pick, act)

Deliverable: full mouse navigation and selection parity with the GTK build.

### Task 4.1: input.cpp — port viewport.c event handling

**Files:**
- Create: `src/sdl/input.cpp` (+ `input.h`: `void input_handle_event(const SDL_Event *ev);`)
- Modify: `src/sdl/main.cpp` (route events)

Port the bindings from `src/viewport.c` and the original man page semantics:
- Left click: select node under cursor (uses `gpu_pick`) + `camera_look_at(node)`.
- Left double-click: activate (warp into dir / open file).
- Middle drag: fly — direction/speed from drag delta (port the exact math from viewport.c); Shift+middle adds vertical motion.
- Right click: ImGui context menu (Task 5.1 provides the menu content; until then, log).
- Scroll wheel: `camera_dolly(delta)`.

- [x] **Step 1: Implement, compile, manual test each binding. Commit** — `git commit -m "feat: port mouse navigation and selection input"`.

**Deviation:** the bindings above were this plan's *assumption*, written
before `viewport.c` was read in full; the real gestures (PORTING.md,
Task 4.1) are middle-drag dolly and Ctrl+left-drag revolve, not a
middle-drag "flight" with Shift for vertical motion — `viewport.c` has no
flight mechanic and no Shift modifier anywhere. Left double-click has no
"activate" behavior in the 3D viewport either (that lives in `dirtree.c`,
ported separately in Task 5.2); a double-click is just two ordinary
clicks in a row, exactly as upstream. Scroll-wheel dolly was ported as a
labeled *addition* (upstream has no wheel gesture at all), not a port.

### Task 4.2: gpu_pick — color-ID picking readback

**Files:**
- Modify: `src/sdl/gpu.cpp`

- [x] **Step 1: Implement `gpu_pick(x, y)`** as a direct port of `ogl_select_modern()` (`src/ogl.c:456`): render `geometry_draw(FALSE)` with the picking pipeline into a private RGBA8 render target, then `SDL_DownloadFromGPUTexture` the single texel `(x, height-1-y)` region via a transfer buffer, `SDL_MapGPUTransferBuffer`, decode `id = r | g<<8 | b<<16`. Note in a comment: full-target download is acceptable at first (picking is click-frequency); optimize to a 1×1 region only if profiling demands.
- [x] **Step 2: Manual test — click each of ~10 nodes in the fixture tree; selection highlight and dirtree state must match. Commit** — `git commit -m "feat: color-ID picking via GPU readback"`.

---

## Milestone 5 — UI parity with ImGui

Deliverable: menu bar, directory tree panel, file list, color-setup and properties dialogs; GTK-free feature parity; `frontend=sdl` becomes the macOS default.

### Task 5.1: ui_main.cpp — menu bar and mode switching (replaces gui.c/window.c shell)

- Menus to reproduce (from `src/gui.c` menu construction): **File** (Change root…, Rescan, Exit), **Vis** (DiscV/MapV/TreeV radio → `fsv_set_mode()`), **Colors** (by node type / timestamp / wildcard → dialog), **Help** (About, Controls).
- "Change root…" uses ImGui's file-browser-less approach: SDL3's `SDL_ShowOpenFolderDialog` (native macOS panel).

- [x] Implement + commit — `git commit -m "feat: ImGui menu bar and mode switching"`.

### Task 5.2: ui_panels.cpp — directory tree + file list (replaces dirtree.c/filelist.c)

**Interfaces:**
- Consumes: `globals.fstree` (GNode tree), `NODE_DESC(node)` accessors from `fsv.h`/`common.h`, selection state via existing core calls (`camera_look_at`, colexp expand/collapse API from `colexp.h`).

Representative pattern (dir tree):

```cpp
static void draw_dir_node(GNode *gnode)
{
	NodeInfo *ni = NODE_INFO(gnode);   // exact accessor per fsv.h
	ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_OpenOnArrow |
	    (gnode == g_selected ? ImGuiTreeNodeFlags_Selected : 0) |
	    (node_has_subdirs(gnode) ? 0 : ImGuiTreeNodeFlags_Leaf);
	bool open = ImGui::TreeNodeEx(ni, fl, "%s", ni->name);
	if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
		{ g_selected = gnode; camera_look_at(gnode); }
	if (open) {
		for (GNode *c = gnode->children; c; c = c->next)
			if (node_is_dir(c)) draw_dir_node(c);
		ImGui::TreePop();
	}
}
```

- [x] Implement both panels in a left dock (ImGui docking), sync expand/collapse with `colexp` so tree state and 3D state stay coherent (as GTK's dirtree.c does today). Commit per panel.

### Task 5.3: ui_dialogs.cpp — color setup, properties, about

- Port `src/dialog.c` logic (not widgets): color-by-timestamp spectrum settings, color-by-wildcard-pattern list (add/edit/remove rows), node properties window (name, size, mtime, owner — data already computed in common.c).
- About window: text + version; drop the GL splash of `about.c` (keep the splash *mode* removal noted in PORTING.md).

- [x] Implement + commit each dialog. Then flip the meson default: `frontend` default `sdl` when `host_machine.system() == 'darwin'`. Commit — `git commit -m "feat: ImGui dialogs; make SDL frontend the macOS default"`.

**Deviation, disclosed as a real find, not a plan miss:** `lib/nvstore.c`
— the only persistence backend `color.c` uses, on *both* frontends — was
a complete upstream stub (`nvs_open()` always returned `NULL`; every
read/write was a no-op). Color-setup persistence was therefore
unimplementable on either frontend until this task implemented
`nvstore.c` for real (an in-memory tree serialized to `~/.fsvrc`, zero
new dependencies). This also fixes the same latent gap in the GTK
frontend, whose own (dead-code, `#if 0`'d) config path would now work
too. See PORTING.md, Task 5.3.

---

## Milestone 6 — Packaging, CI, docs

### Task 6.1: Xcode project (no signed bundle)

No paid Apple Developer account is available, so we do NOT ship a signed
`.app` bundle. Instead, provide a minimal Xcode project alongside the Meson
build so users who want a bundled app can build one themselves.

**Files:**
- Create: `packaging/xcode/fsv.xcodeproj/project.pbxproj` — an Xcode project whose single target is an **External Build System** target driving Meson/ninja (`meson setup builddir-xcode -Dfrontend=sdl && ninja -C builddir-xcode`), so there is one source of truth for the build. Signing settings left at "Sign to Run Locally" (ad-hoc, no team).
- Create: `packaging/xcode/README.md` — how to open, build, and (optionally) sign with your own team if you have one.
- Create: `packaging/macos/Info.plist` + `packaging/macos/fsv.icns` — referenced by the Xcode target's bundle step for local use.

- [x] Generate the project, verify `xcodebuild -project packaging/xcode/fsv.xcodeproj -scheme fsv build` succeeds on a clean checkout with only brew deps installed. Commit.

**Deviation:** no `packaging/macos/fsv.icns` — the repo's only icon asset
is a legacy GTK XPM (`src/xmaps/fsv-icon.xpm`), not a viable `.icns`
source without a hand-drawn multi-resolution PNG set; skipped per this
task's own "optional" carve-out (`make-bundle.sh` picks one up
automatically if ever added). The target itself is a `PBXLegacyTarget`
("External Build System"), with no Signing & Capabilities tab — such
targets have no product for Xcode to sign, so ad-hoc signing lives in
`packaging/macos/make-bundle.sh` instead, as this task's plan text itself
anticipated.

### Task 6.2: GitHub Actions CI

**Files:**
- Create: `.github/workflows/ci.yml`

```yaml
name: ci
on: [push, pull_request]
jobs:
  macos-metal:
    runs-on: macos-14
    steps:
      - uses: actions/checkout@v4
      - run: brew install glib cglm meson ninja pkgconf sdl3
      - run: meson setup builddir -Dfrontend=sdl
      - run: ninja -C builddir
      - run: meson test -C builddir --print-errorlogs
      - uses: actions/upload-artifact@v4
        with: { name: fsv-macos, path: builddir/src/sdl/fsv }
  linux-core:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - run: sudo apt-get update && sudo apt-get install -y meson ninja-build libglib2.0-dev libcglm-dev libgtk-3-dev libepoxy-dev
      - run: meson setup builddir -Dfrontend=gtk
      - run: ninja -C builddir && meson test -C builddir --print-errorlogs
```

(Note: GPU rendering can't run on CI runners; CI covers build + headless core tests. The `--screenshot` smoke test is a local/manual gate.)

- [x] Commit — `git commit -m "ci: macOS Metal build + Linux GTK build"`.

**Deviation — the task list changed shape here.** The shipped
`.github/workflows/ci.yml` has four jobs, not the two sketched above:
`macos-metal`, `linux-gtk` (the anti-regression job — meson's default
frontend became `sdl` in Task 5.3, so this job must pass
`-Dfrontend=gtk` explicitly or it would silently stop exercising GTK at
all), `linux-sdl` (best-effort, `continue-on-error: true` — SDL3 has no
Ubuntu package on any GitHub-hosted runner as of this task, so it's
built from source and cached), and `release` (Task 6.3's job, folded
into the same workflow file rather than a separate change). Runner image
is `macos-15`, not `macos-14` — `macos-14`'s image began deprecating
during this branch's likely CI lifetime. See PORTING.md, Tasks 6.2+6.3.

### Task 6.3: Release artifacts in CI (user request, 2026-08-07)

GitHub Actions is free for public repos; `ubuntu-latest` = Linux x86_64,
`macos-14`+ = Apple Silicon (arm64). Extend the CI workflow:

- [x] macOS job uploads the built `fsv` binary (arm64) as an artifact on every push; Linux job builds the SDL frontend too (`-Dfrontend=sdl`, Vulkan available headless for build only) and uploads the x86_64 binary.
- [x] Add a `release` job triggered on tag push (`v*`): repackages both artifacts (tar.gz with shaders/ and README) and attaches them to a GitHub Release via `softprops/action-gh-release` (or `gh release upload`).
- [x] Commit — `git commit -m "ci: attach Linux x86_64 and macOS arm64 binaries to releases"`.

**Deviations:** release tarballs deliberately do NOT contain a `shaders/`
directory (unlike this task's own text above) — shaders are compiled
offline and embedded in the binary since Task 3.2, so nothing under
`shaders/` is needed at runtime on either platform. Release publication
uses the `gh` CLI directly (`gh release create`/`gh release upload`),
not `softprops/action-gh-release`. The Linux tarball ships the SDL binary
when the best-effort `linux-sdl` job produced one, falling back to the
GTK binary otherwise (both tiers of dependency documented in each
tarball's `USAGE.txt`); a code-review fix round also made the Linux SDL
build link SDL3 **statically** (a first pass linked it shared, which
would not have started on any real Ubuntu download target — see
PORTING.md's Task 6.2+6.3 fix round).

### Task 6.4: Demo video (user request, 2026-08-07)

A ≤20s demo video of navigating this repo's own source tree in fsv, embedded in the README.

- [x] Add a `--record <dir> <seconds> <out-prefix>` mode to the SDL frontend (or a small driver script): drive the camera programmatically (scripted look_at/dolly/revolve sequence over the project's `src/` tree), render frames offscreen via the existing readback path at ~30fps, dump numbered PNGs/BMPs. Screen capture is TCC-blocked in this environment — offscreen rendering is the sanctioned path.
- [x] Assemble with ffmpeg (brew): `demo.mp4` (H.264, ≤20s) AND `demo.gif` (optimized ≤10MB, palette pass) committed under `docs/media/`.
- [x] Embed `docs/media/demo.gif` in README.md near the top. Commit — `git commit -m "docs: add navigation demo video"`.
- [x] Remove the recording mode afterwards ONLY if it required invasive hooks; a clean `--record` flag may stay (useful for future docs).

### Task 6.5: Documentation (amended per user request, 2026-08-07)

- [x] Rewrite `README.md`: what fsv is (fsn lineage), **revised Install section** (macOS: brew deps + meson build + Xcode project pointer; Linux: apt deps, both frontends; link CI release artifacts for prebuilt binaries), **replace the "TODO" section with a "What's been done" section** summarizing the metal-port work (SDL3 GPU/Metal renderer, ImGui UI, preserved GTK frontend, CI), controls table (the REAL gestures from input.cpp), demo GIF embed, screenshots. English.
- [x] Update `docs/PORTING.md` decision log; mark plan checkboxes done.
- [x] Commit — `git commit -m "docs: macOS-first README and porting retrospective"`.

**Deviation:** the Controls table in the rewritten README does not match
the task brief's own pre-reading assumption ("double-click
activate/warp") — reading `src/sdl/input.cpp` in full shows no such
gesture exists anywhere in the 3D viewport, in this port or upstream; see
PORTING.md's Task 6.5 section for the full correction. No separate
"screenshots" section was added beyond the existing demo GIF/mp4 embed
(from Task 6.4) — this task's own scope list already folds screenshots
into that embed rather than asking for a second, static set.

---

## Risks & fallbacks

1. **ImGui SDLGPU3 backend API drift** — the backend is young; pin the vendored version and mirror its bundled example. Fallback: none needed, vendoring isolates us.
2. **SDL_shadercross availability** — only needed when *changing* shaders (artifacts are committed). Fallback: hand-write the 4 MSL files (they are small).
3. **geometry.c port size** (136 call sites) — the mapping table makes it mechanical, but budget it as the single largest task; commit per view-mode section (DiscV/MapV/TreeV) to keep bisectability.
4. **GTK frontend regression** — Tasks 1.1/1.2 touch shared files; the Linux CI leg exists precisely to catch this from M6, and a container build is required at M1 (Task 1.1 Step 3).
5. **camera.c hidden GTK coupling** beyond scrollbars — if found, extend `FsvPlatformHooks` rather than ifdef-ing GTK includes.

## Self-review notes

- Spec coverage: fork ✔ (created), branch ✔ (M0), Metal ✔ (SDL_GPU MSL path, M2–M3), English docs ✔ (M0/M6), frequent commits ✔ (per-step commit discipline).
- Known soft spots called out explicitly rather than hidden: exact `scanfs()`/`NODE_INFO` signatures must be read from headers at implementation time (Tasks 1.3, 5.2); ImGui backend identifiers must be cross-checked against the vendored example (Task 2.2). These are verification steps, not placeholders — the contracts (test assertions, panel behavior) are fixed here.
- Type consistency: `FsvPlatformHooks` (1.1) matches its uses in 1.2/2.2; `gpu.h` API (3.2) matches call sites in 3.3/3.4/4.2.
