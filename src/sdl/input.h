// src/sdl/input.h — SPDX-License-Identifier: MIT
//
// Mouse navigation and node-selection input for the SDL/Metal frontend.
// Port of src/viewport.c's viewport_cb() (GTK's GdkEvent switch) onto
// SDL3's SDL_Event. The GTK arm (src/viewport.c) is untouched; this is
// the SDL-only counterpart, wired into src/sdl/main.cpp's event loop.
//
// See docs/PORTING.md's Task 4.1 section for the full GTK -> SDL gesture
// mapping table and the deviations from viewport.c's exact math.
#pragma once

#include <SDL3/SDL.h>

// Call once per polled SDL_Event, after ImGui_ImplSDL3_ProcessEvent() so
// ImGui's io.WantCaptureMouse already reflects this event. Does nothing
// for a mouse event that starts over an ImGui window; a drag already in
// progress (SDL_CaptureMouse()-held: middle-button dolly, or Ctrl+left
// revolve) keeps running regardless of what the cursor is over, mirroring
// how GTK's implicit pointer grab kept viewport.c's own drags alive past
// the widget's bounds.
//
// No per-frame tick is needed: viewport.c only ever moves the camera from
// GDK_MOTION_NOTIFY's own delta, never from a timer or the busy animation
// loop while a button is held but the mouse is stationary. This function
// is the complete port of that mechanism.
void input_handle_event(const SDL_Event *ev);
