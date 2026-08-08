/* src/gpu.h — SPDX-License-Identifier: MIT
 *
 * The renderer contract the fsv core draws against.
 *
 * `geometry.c` is shared by both frontends and contains no GL and no GTK;
 * every drawing primitive it needs is declared here and implemented once
 * per frontend:
 *
 *   SDL/Metal frontend : src/sdl/gpu.cpp     (SDL_GPU)
 *   GTK frontend       : src/ogl-gpu-compat.c (epoxy/OpenGL 3.1)
 *
 * Deliberately a *pure C* header (extern "C" below): geometry.c is C.
 * Anything that needs SDL types lives in src/sdl/gpu_internal.hpp
 * instead, which C code never sees.
 *
 * Mapping from the OpenGL frontend this replaces (src/ogl.h, src/ogl.c):
 *
 *   glGenBuffers/glBufferData/glDrawArrays/glDrawElements -> gpu_draw( )
 *   glVertexAttribPointer + glEnableVertexAttribArray     -> FsvVertex
 *   gl.projection / gl.modelview        -> gpu_mat.projection / .modelview
 *   ogl_upload_matrices( )              -> gpu_upload_matrices( )
 *   glUniform4fv(gl.color_location, .)  -> gpu_set_color( )
 *   ogl_enable/disable_lightning( )     -> gpu_set_lighting( )
 *   glDepthFunc( )                      -> gpu_set_depth_test( )
 *   glLineWidth( )                      -> gpu_set_line_width( )
 *   gl.render_mode                      -> gpu_render_mode( )
 *   ogl_select_modern( )                -> gpu_pick( )
 */

#ifndef FSV_GPU_H
#define FSV_GPU_H

#include <cglm/cglm.h>

#ifdef __cplusplus
extern "C" {
#endif

/**** Geometry submission ****************/

/* One vertex, exactly as the scene pipeline's vertex input state
 * describes it: 24-byte stride, two attributes (locations 0 and 1),
 * matching `in vec3 position` / `in vec3 normal` in shaders/src/scene.vert.
 * Line geometry leaves `normal` zeroed and draws with lighting off, as
 * the GL frontend's position-only vertex arrays did. */
typedef struct {
	float pos[3];
	float normal[3];
} FsvVertex;

/* The GL primitive modes geometry.c actually draws with.
 *
 * SDL_GPU bakes the primitive type into the pipeline object and offers
 * neither TRIANGLE_FAN nor LINE_LOOP, so the renderer converts: fans and
 * strips become indexed triangle lists, loops and strips become indexed
 * line lists. That conversion is the renderer's business, not
 * geometry.c's — hence a topology argument rather than six entry points.
 * The GL compat shim maps these straight back onto GL_* modes. */
typedef enum {
	FSV_TRIANGLES,      /* GL_TRIANGLES */
	FSV_TRIANGLE_FAN,   /* GL_TRIANGLE_FAN */
	FSV_TRIANGLE_STRIP, /* GL_TRIANGLE_STRIP */
	FSV_LINES,          /* GL_LINES */
	FSV_LINE_STRIP,     /* GL_LINE_STRIP */
	FSV_LINE_LOOP       /* GL_LINE_LOOP */
} FsvTopology;

/* Draws one batch of vertices with the current color / lighting /
 * depth-test / matrix state. Replaces the glBufferData + glDrawArrays
 * (indices == NULL, the glDrawArrays case) and glBufferData +
 * glDrawElements (indices != NULL) blocks geometry.c used to inline at
 * every call site.
 *
 * The caller keeps ownership of both arrays and may free or reuse them
 * the moment this returns; the renderer copies whatever it still needs.
 *
 * Framing is per-backend, because the two backends need different things
 * from the caller:
 *
 *  - SDL_GPU (src/sdl/gpu.cpp): must be called between gpu_scene_begin( )
 *    and gpu_scene_end( ). Draws are recorded and replayed at scene end,
 *    since SDL_GPU forbids buffer uploads inside a render pass; a call
 *    made outside that bracket is dropped.
 *  - GTK/OpenGL (src/ogl-gpu-compat.c): neither implements nor requires
 *    the bracket -- GL draws immediately into the bound framebuffer, and
 *    viewport.c has already set that up. The two functions below are
 *    declared unconditionally but that backend defines neither, which
 *    links only because the caller is the frontend, never geometry.c:
 *    viewport.c's draw callback is the GTK frame boundary, and only
 *    src/sdl/main.cpp calls gpu_scene_begin/end.
 *
 * Note this rebuilds vertex data every frame, which is what the GL
 * frontend did too (one shared streaming VBO per call site, re-specified
 * per draw). Caching per-node geometry is a separate concern; the "draw
 * stage" bookkeeping geometry.c already carries is where that would go. */
void gpu_draw(FsvTopology topology, const FsvVertex *verts, int nverts,
              const unsigned int *indices, int nindices);

/**** Device / frame ****************/

/* Creates the GPU device and claims `sdl_window` (an SDL_Window *; typed
 * void * so this header stays free of SDL headers for C callers), builds
 * the scene pipelines, and initializes the camera matrices.
 * SDL frontend only — the GTK shim has no equivalent (ogl.c's
 * realize_cb/ogl_init do this) and does not implement it. */
void gpu_init(void *sdl_window);
void gpu_shutdown(void);

/* Brackets the scene. gpu_scene_begin( ) refreshes the camera matrices
 * from camera.c and resets the frame's geometry buffers; the gpu_draw( )
 * calls in between are recorded; gpu_scene_end( ) uploads the recorded
 * geometry in one transfer and replays the draws into the render pass
 * (clearing color + depth first).
 *
 * The work therefore happens at gpu_scene_end( ), not gpu_scene_begin( ):
 * SDL_GPU forbids buffer copies inside a render pass, and the vertex data
 * for a frame is only fully known once geometry_draw( ) has returned. See
 * the "Recording and replay" note at the top of src/sdl/gpu.cpp. */
void gpu_scene_begin(void);
void gpu_scene_end(void);

/* Color-ID picking: renders the geometry with id-colors offscreen and
 * reads back one pixel. Port of ogl_select_modern() (src/ogl.c:456).
 * Returns the node id, or 0 for "nothing there". */
unsigned int gpu_pick(int x, int y);

/**** Draw state ****************/

/* Whether draws paint their real colors or flat per-node id colors.
 * geometry.c's node_set_color( ) branches on this exactly as it branched
 * on gl.render_mode. Picking (Task 4.2) is what puts the renderer into
 * FSV_RENDER_SELECT for the duration of one offscreen pass. */
typedef enum {
	FSV_RENDER_NORMAL = 0,
	FSV_RENDER_SELECT
} FsvRenderMode;

FsvRenderMode gpu_render_mode(void);

/* Fill color for subsequent draws (glUniform4fv on gl.color_location). */
void gpu_set_color(float r, float g, float b, float a);

/* Phong lighting on/off for subsequent draws (ogl_enable_lightning( ) /
 * ogl_disable_lightning( )). Feeds both the vertex and the fragment
 * uniform block — SDL_GPU has no shared cross-stage uniform namespace,
 * see the note in shaders/src/scene.vert. */
void gpu_set_lighting(int enabled);

/* Depth comparison for subsequent draws (glDepthFunc). Everything draws
 * with FSV_DEPTH_LESS — GL's default — except the node cursor, which
 * draws its occluded half with FSV_DEPTH_GREATER and its visible half
 * with FSV_DEPTH_LEQUAL (src/geometry.c, cursor_hidden_part( ) /
 * cursor_visible_part( )), and the fsn-mode landscape sky (Task A1,
 * src/sdl/gpu.cpp's draw_landscape( )), which uses FSV_DEPTH_ALWAYS_NOWRITE
 * below.
 *
 * FSV_DEPTH_ALWAYS_NOWRITE: the depth test always passes *and* nothing is
 * written to the depth buffer -- the standard "background/skybox" depth
 * mode. A fixed NDC-depth trick (park the backdrop just under the far
 * clip value) was tried first and rejected: MapV/TreeV's near:far ratio
 * is 128:1, and glm_frustum_rh_zo's projection is non-linear enough in Z
 * that any fixed NDC depth close to 1.0 also falls within the *linear*
 * (world-space) depth range real geometry can legitimately occupy near
 * the far clip plane -- which would make the backdrop wrongly occlude
 * that geometry instead of always losing to it. Disabling the test
 * outright has no such failure mode regardless of the projection's
 * shape, and needs no per-scene tuning.
 *
 * FSV_DEPTH_LESS_NOWRITE (fsn-mode Task B3, src/geometry-fsn-draw.c's
 * selection spotlight): the opposite trade-off from
 * FSV_DEPTH_ALWAYS_NOWRITE above -- a translucent decal that DOES want
 * to respect the depth buffer (so it loses to real geometry standing
 * between it and the camera, the same as any opaque draw) but must
 * never leave a depth value behind, both so it never occludes anything
 * drawn after it in the same frame and so several overlapping decal
 * layers blend against each other by draw order rather than fighting
 * each other's depth. This is also the one FsvDepthTest value that pairs
 * with alpha blending: src/sdl/gpu.cpp's pipeline_for() enables blending
 * (SRC_ALPHA / ONE_MINUS_SRC_ALPHA, the same factors as the text
 * pipeline) only for this variant, since the spotlight is currently its
 * only caller and gpu_draw()'s contract has no separate knob for blend
 * state -- see that function's own comment for why tying the two
 * together here was chosen over adding one. */
typedef enum {
	FSV_DEPTH_LESS = 0,
	FSV_DEPTH_LEQUAL,
	FSV_DEPTH_GREATER,
	FSV_DEPTH_ALWAYS_NOWRITE,
	FSV_DEPTH_LESS_NOWRITE
} FsvDepthTest;

void gpu_set_depth_test(FsvDepthTest test);

/* Line width for subsequent line draws (glLineWidth). Honored by the GL
 * shim; ignored by the SDL_GPU backend, which rasterizes every line one
 * pixel wide and has no equivalent knob — see docs/PORTING.md. */
void gpu_set_line_width(float width);

/**** Landscape (fsn-mode Task A1: sky/ground presets) ****************/

/* Selects a landscape preset by index into fsn_landscapes[] (src/
 * fsn-style.h), or FSN_LANDSCAPE_OFF (-1) to disable landscape drawing
 * entirely (today's plain flat clear, unchanged from Task 2.2). The
 * caller (src/color.c's landscape_set()/landscape_init()) is also the
 * one that persists the choice via nvstore, by preset name rather than
 * this raw index -- see fsn-style.h's FsnLandscape::name.
 *
 *   SDL/Metal (src/sdl/gpu.cpp): draws a screen-filling sky gradient and
 *   a large ground quad in the scene pass, before geometry_draw()'s own
 *   draws, and skips both entirely in FSV_RENDER_SELECT mode -- the sky
 *   and ground are not pickable, so clicking on either must resolve to
 *   node id 0 ("nothing there"), exactly like today's empty background.
 *
 *   GTK/OpenGL (src/ogl-gpu-compat.c): no-op. The GTK frontend keeps its
 *   pre-A1 flat clear regardless of the stored preference -- see
 *   docs/PORTING.md's "fsn mode" section for why. */
void gpu_set_landscape(int index);

/**** Camera matrices ****************/

/* The renderer-agnostic counterpart of FsvGlState.projection /
 * .modelview (src/ogl.h). `modelview` is public because geometry.c saves,
 * translates/rotates/scales and restores it around every subtree it draws
 * and then calls gpu_upload_matrices( ). `projection` is public because
 * it is where each backend's projection setup puts its result and what
 * gpu_upload_matrices( ) multiplies by. Call gpu_upload_matrices( ) after
 * mutating either. */
typedef struct {
	mat4 projection;
	mat4 modelview;
} FsvGpuMatrices;

extern FsvGpuMatrices gpu_mat;

/* Recomputes the derived matrices (mvp, normal matrix) from gpu_mat and
 * stages them for the next draw call. Replaces ogl_upload_matrices( ). */
void gpu_upload_matrices(void);

/**** Texture-mapped text (src/tmaptext.c) ****************/
/*
 * tmaptext.c is shared by both frontends, exactly like geometry.c: it
 * keeps its font-atlas generation and glyph-layout math (get_char_dims( ),
 * get_char_tex_coords( ), text_draw_straight/_rotated/_curved( )) and
 * draws through this small extra slice of the same contract instead of
 * owning a GL texture/program of its own. See docs/PORTING.md Task 3.4
 * for why this file was ported in place rather than split into a new
 * src/sdl/text3d.cpp.
 *
 *   glGenTextures/glTexImage2D/glGenerateMipmap  -> gpu_text_init( )
 *   text_pre( ) / text_post( )                   -> gpu_text_begin( ) / gpu_text_end( )
 *   glBufferData + glDrawElements (text VBO/EBO)  -> gpu_text_draw( )
 *   glUniform3f(glt.color_location, ...)          -> gpu_text_set_color( )
 *   glUniformMatrix4fv(glt.mvp_location, ...)     -> gpu_text_upload_mvp( )
 */

/* One glyph-quad vertex, matching shaders/src/text.vert's `position`
 * (world space, shares the scene's mvp) / `texcoord` (into the atlas)
 * inputs -- tmaptext.c's old TextVertex struct, renamed and moved here
 * because it is now the vertex format of a cross-file contract. */
typedef struct {
	float pos[3];
	float texcoord[2];
} FsvTextVertex;

/* Uploads the glyph atlas: a single-channel bitmap, row-major,
 * `pixels[y*width+x]` in [0,255], sampled as alpha by shaders/src/
 * text.frag's `alpha.r`. Call once, from text_init( ). */
void gpu_text_init(const unsigned char *pixels, int width, int height);

/* Brackets one batch of gpu_text_draw( ) calls -- what text_pre( ) /
 * text_post( ) used to do directly (glDisable(GL_POLYGON_OFFSET_FILL),
 * glEnable(GL_BLEND), bind/unbind the atlas texture). The SDL_GPU backend
 * bakes blend state and the absence of depth bias into its text pipeline,
 * so these are a no-op there; the GL compat shim still does the exact
 * old state dance. */
void gpu_text_begin(void);
void gpu_text_end(void);

/* Draws one batch of glyph quads -- 4 vertices / char, indices in the
 * same LL-LR-UL-UR-per-char order tmaptext.c's draw_text_vertices( )
 * built -- with the current text color and mvp (gpu_text_set_color( ) /
 * gpu_text_upload_mvp( )). Same recording contract as gpu_draw( ) on the
 * SDL_GPU backend (must be called between gpu_scene_begin( ) and
 * gpu_scene_end( ), and it is *this* call that snapshots the current
 * color/mvp -- both can and do change between calls within one high-
 * detail pass, once per node); draws immediately on the GL compat shim. */
void gpu_text_draw(const FsvTextVertex *verts, int nverts,
                    const unsigned int *indices, int nindices);

/* Text tint, per label (glUniform3f on the old glt.color_location). */
void gpu_text_set_color(float r, float g, float b);

/* Text mvp. Independent of gpu_upload_matrices( )'s scene mvp in
 * principle, but every real caller keeps them equal: geometry.c places
 * labels in world space (not billboards), and gpu_upload_matrices( )
 * itself calls text_upload_mvp( ) with the same matrix right after
 * updating the scene one, so a label always shares its node's current
 * transform. about_splash_draw( ) (GTK only, no SDL_GPU equivalent) is
 * the one place that calls text_upload_mvp( ) directly with a different,
 * orthographic matrix. */
void gpu_text_upload_mvp(const float *mvp);

#ifdef __cplusplus
}
#endif

#endif /* FSV_GPU_H */
