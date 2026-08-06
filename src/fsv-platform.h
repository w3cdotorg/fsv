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
