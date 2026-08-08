// src/sdl/gpu_internal.hpp — SPDX-License-Identifier: MIT
//
// The parts of the gpu.cpp renderer that speak SDL types, and so cannot
// live in gpu.h (which stays pure C for geometry.c). Only main.cpp uses
// this: gpu.cpp owns the device, the per-frame command buffer and the
// swapchain texture, but ImGui's backend needs all three to render its
// own pass on top of the scene.
//
// Frame flow in main.cpp:
//
//   cmd = gpu_frame_begin();          // acquire cmd buffer + swapchain
//   ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, cmd);   // copy pass
//   gpu_scene_begin();                // pass 1: CLEAR color + depth
//   ...geometry draws...
//   gpu_scene_end();
//   ...pass 2: LOAD the same swapchain texture, render ImGui...
//   gpu_frame_end();                  // submit
#pragma once

#include <SDL3/SDL.h>

// The device created by gpu_init(). NULL before gpu_init()/after
// gpu_shutdown().
SDL_GPUDevice *gpu_device(void);

// True only once gpu_init() has completed every step (device, window
// claim, depth format, shaders, both pipelines). gpu_init() itself
// returns void because gpu.h is the C contract geometry.c compiles
// against; main.cpp checks this instead and exits with the single error
// gpu_init() already logged, rather than running on with a half-built
// renderer that re-logs the same failure every frame.
bool gpu_ready(void);

// Acquires this frame's command buffer and swapchain texture. Returns
// the command buffer, or nullptr if one could not be acquired (in which
// case the frame must be skipped entirely — do not call gpu_frame_end()).
SDL_GPUCommandBuffer *gpu_frame_begin(void);

// This frame's swapchain texture, or nullptr when the swapchain had no
// image available (window occluded, or the window is being resized).
// Everything that draws must be skipped in that case; gpu_frame_end()
// must still be called to submit (and thereby release) the command
// buffer.
SDL_GPUTexture *gpu_frame_swapchain_texture(void);

// Submits this frame's command buffer.
void gpu_frame_end(void);

// --screenshot: render one scene into an offscreen R8G8B8A8 texture
// instead of the swapchain and write it out as a BMP. The caller drives
// the scene between the two calls:
//
//   gpu_screenshot_begin(w, h);
//   gpu_scene_begin(); geometry_draw(TRUE); gpu_scene_end();
//   gpu_screenshot_end("out.bmp");
//
// Both return false (having logged the reason) if the capture could not
// be set up or written; gpu_screenshot_end() always releases the texture.
bool gpu_screenshot_begin(int width, int height);
bool gpu_screenshot_end(const char *path);

// --record (Task 6.4): like gpu_screenshot_begin/end() above, but the
// offscreen texture is created in the swapchain's own pixel format
// instead of a fixed R8G8B8A8_UNORM, and the scene pass reuses the
// swapchain's pipelines -- see the "--record" comment in gpu.cpp. That
// format match is what lets the caller also render ImGui's draw data
// into this texture with ImGui's one already-built pipeline, so a
// recorded frame can composite scene + ImGui exactly like a visible
// frame does:
//
//   SDL_GPUCommandBuffer *cmd = gpu_record_begin(w, h);
//   gpu_scene_begin(); geometry_draw(TRUE); gpu_scene_end();
//   // ImGui pass on `cmd`, target = gpu_record_texture(), LOADOP_LOAD
//   gpu_record_end("frame.bmp");
//
// gpu_record_begin() returns nullptr (having logged the reason) on
// failure, exactly like gpu_frame_begin() -- do not call gpu_record_end()
// in that case. gpu_record_end() always releases the texture.
SDL_GPUCommandBuffer *gpu_record_begin(int width, int height);
SDL_GPUTexture *gpu_record_texture(void);
bool gpu_record_end(const char *path);

// fsn-mode Task C1: renders the FSN landscape from straight above, at a
// fixed FSN_OVERVIEW_WIDTH x FSN_OVERVIEW_HEIGHT (src/fsn-style.h), into
// a cached texture ImGui shows in the overview window
// (src/sdl/ui_overview.cpp), with a marker at the live camera's ground
// position. Scene only: no ImGui, no labels, no spotlight.
//
// MUST be called inside a frame, on that frame's command buffer
// (i.e. after gpu_frame_begin()/gpu_record_begin() and before
// gpu_frame_end()), and before the ImGui pass that samples the result --
// SDL_GPU orders passes within a command buffer, so a render issued here
// is guaranteed complete by the time ImGui's pass reads the texture. It
// is NOT part of the visible scene pass and does not disturb it: it opens
// and closes its own render pass, against its own color and depth
// targets, leaving --screenshot/--record's capture state untouched (see
// gpu.cpp's g_overview_texture comment).
//
// Returns false, having drawn nothing, when there is no FSN layout to
// frame, when the current mode is not FSV_FSN, or when called outside a
// frame. The caller decides *when* to call it -- the overview is
// redrawn on camera/layout change, not every frame.
bool gpu_overview_render(void);

// The mini-map texture, or nullptr until gpu_overview_render() has
// succeeded at least once (before that it holds undefined pixels and
// must not be shown).
SDL_GPUTexture *gpu_overview_texture(void);

// The world-space ground rectangle (x0,x1) x (y0,y1) that the last
// successful gpu_overview_render() framed -- what src/sdl/ui_overview.cpp
// maps a click inside the image back through. Any argument may be NULL.
// All zero before the first successful render.
void gpu_overview_frame_rect(double *x0, double *x1, double *y0, double *y1);
