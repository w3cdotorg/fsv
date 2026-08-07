/* src/sdl/gpu.h — SPDX-License-Identifier: MIT
 *
 * SDL_GPU (Metal on macOS) renderer core: device, scene pipeline, mesh
 * buffers, camera matrices. This is the replacement for src/ogl.c in the
 * SDL frontend.
 *
 * Deliberately a *pure C* header (extern "C" below): geometry.c is C and
 * calls straight into this API once its GL calls are gone (Task 3.3). The
 * implementation is C++ (src/sdl/gpu.cpp); anything that needs SDL types
 * lives in src/sdl/gpu_internal.hpp instead, which C code never sees.
 *
 * Mapping from the OpenGL frontend (src/ogl.h, src/ogl.c):
 *
 *   glGenBuffers + glBufferData         -> fsv_mesh_new + fsv_mesh_upload
 *   glDrawElements(GL_TRIANGLES, ...)   -> fsv_mesh_draw
 *   gl.projection / gl.modelview        -> gpu_mat.projection / .modelview
 *   ogl_upload_matrices( )              -> gpu_upload_matrices( )
 *   glUniform4fv(gl.color_location, .)  -> gpu_set_color( )
 *   ogl_enable/disable_lightning( )     -> gpu_set_lighting( )
 *   ogl_select_modern( )                -> gpu_pick( )
 */

#ifndef FSV_SDL_GPU_H
#define FSV_SDL_GPU_H

#include <cglm/cglm.h>

#ifdef __cplusplus
extern "C" {
#endif

/**** Meshes ****************/

/* Opaque handle to a GPU vertex + index buffer pair. */
typedef struct FsvMesh FsvMesh;

/* One vertex as the scene pipeline's vertex input state describes it:
 * 40-byte stride, 3 attributes (locations 0, 1, 2). `color` is per-vertex
 * paint for the future; the ported scene.vert/scene.frag currently take
 * the fill color from a per-draw uniform (gpu_set_color) exactly as the
 * GL frontend did, so location 2 is declared but unread today. */
typedef struct {
	float pos[3];
	float normal[3];
	float color[4];
} FsvVertex;

FsvMesh *fsv_mesh_new(void);
void fsv_mesh_free(FsvMesh *m);

/* Replaces glBufferData on GL_ARRAY_BUFFER/GL_ELEMENT_ARRAY_BUFFER.
 * Buffers are (re)allocated only when they need to grow; meshes are
 * rebuilt on filesystem rescan and are camera-independent, so this is
 * "upload once, draw many". Safe to call outside a frame: it runs its own
 * command buffer + copy pass. */
void fsv_mesh_upload(FsvMesh *m, const FsvVertex *verts, int nverts,
                     const unsigned int *indices, int nindices);

/* Replaces glDrawElements(GL_TRIANGLES, ...). Records into the render
 * pass opened by gpu_scene_begin( ); a no-op (with an assertion in debug
 * builds) if no pass is active. Uses the current gpu_set_color( ) /
 * gpu_set_lighting( ) / gpu_upload_matrices( ) state. */
void fsv_mesh_draw(FsvMesh *m);

/* Same, but paints the mesh in the flat color that encodes `id` for
 * color-ID picking (the RENDERMODE_SELECT path of src/geometry.c), with
 * lighting forced off. The color/lighting state set by the caller is
 * restored afterwards. */
void fsv_mesh_draw_id(FsvMesh *m, unsigned int id);

/**** Device / frame ****************/

/* Creates the GPU device and claims `sdl_window` (an SDL_Window *; typed
 * void * so this header stays free of SDL headers for C callers), builds
 * the scene pipelines, and initializes the camera matrices. */
void gpu_init(void *sdl_window);
void gpu_shutdown(void);

/* Scene render pass: begins the pass on the swapchain texture (clearing
 * color + depth), binds the scene pipeline and refreshes the camera
 * matrices from camera.c. The ImGui pass runs afterwards, in main.cpp.
 * Both are no-ops when the swapchain image is unavailable. */
void gpu_scene_begin(void);
void gpu_scene_end(void);

/* Color-ID picking: renders the geometry with id-colors offscreen and
 * reads back one pixel. Port of ogl_select_modern() (src/ogl.c:456).
 * Returns the node id, or 0 for "nothing there". */
unsigned int gpu_pick(int x, int y);

/**** Camera matrices ****************/

/* The SDL_GPU counterpart of FsvGlState.projection / .modelview
 * (src/ogl.h). Public for the same reason they were public there:
 * geometry.c saves, translates/rotates/scales and restores `modelview`
 * around each subtree it draws, and reads `projection` to compute an MVP
 * for its own culling math (src/geometry.c:995). Call
 * gpu_upload_matrices( ) after mutating either. */
typedef struct {
	mat4 projection;
	mat4 modelview;
} FsvGpuMatrices;

extern FsvGpuMatrices gpu_mat;

/* Recomputes the derived matrices (mvp, normal matrix) from gpu_mat and
 * stages them for the next draw call. Replaces ogl_upload_matrices( ) —
 * with no `text` argument: the text overlay pipeline is a separate
 * module (Task 3.4) and will hook itself in here. */
void gpu_upload_matrices(void);

/* Fill color for subsequent draws (glUniform4fv on gl.color_location). */
void gpu_set_color(float r, float g, float b, float a);

/* Phong lighting on/off for subsequent draws (ogl_enable_lightning( ) /
 * ogl_disable_lightning( )). Feeds both the vertex and the fragment
 * uniform block — SDL_GPU has no shared cross-stage uniform namespace,
 * see the note in shaders/src/scene.vert. */
void gpu_set_lighting(int enabled);

#ifdef __cplusplus
}
#endif

#endif /* FSV_SDL_GPU_H */
