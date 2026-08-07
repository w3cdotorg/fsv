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
