/* src/ogl-gpu-compat.c — SPDX-License-Identifier: MIT
 *
 * The GTK frontend's implementation of src/gpu.h, over epoxy/OpenGL 3.1.
 *
 * geometry.c is shared by both frontends and no longer contains any GL;
 * it draws through gpu.h, which src/sdl/gpu.cpp implements with SDL_GPU
 * (Metal/Vulkan) and this file implements with the exact GL calls that
 * were removed from geometry.c. Every function here is a transcription of
 * the block it replaces -- same GL_STREAM_DRAW hint on the vertex buffer,
 * same attribute setup, same "re-specify the buffer as empty afterwards to
 * avoid an implicit sync" trick -- so the GTK frontend renders what it did
 * before the port.
 *
 * One deliberate difference from the old code: the index data. geometry.c
 * used to build each indexed draw's element buffer once, as GL_STATIC_DRAW,
 * and keep it in a per-call-site static. gpu_draw() takes the indices as a
 * plain argument the caller may free on return, so this file re-uploads
 * them into one shared streaming EBO on every indexed draw instead. That is
 * a few hundred bytes per draw on the four indexed call sites, which is far
 * below the vertex traffic already going over the same path -- caching them
 * back would mean keying a cache off caller identity, which is exactly the
 * complexity the immediate-mode contract exists to avoid.
 *
 * Three notes on the shape of it:
 *
 *  - Uniform state (color, lighting) is cached here and applied inside
 *    gpu_draw(), because that is what the original drawVertex()/
 *    drawVertexPos() helpers in geometry.c did: bind the program, push
 *    the color and lighting uniforms, draw, unbind. Callers may therefore
 *    call gpu_set_color()/gpu_set_lighting() with no program bound.
 *
 *  - Depth function and line width are plain GL server state, not program
 *    state, so those two are applied immediately.
 *
 *  - gpu_mat is the single storage for the projection/modelview matrices,
 *    for this frontend too: ogl.c writes them in setup_*_matrix() and
 *    geometry.c pushes/pops modelview around every subtree. Keeping a
 *    second copy in FsvGlState would be two sources of truth for the same
 *    matrix.
 *
 * gpu_init()/gpu_shutdown() are deliberately not implemented: device and
 * pipeline creation is ogl.c's realize_cb()/ogl_init(), and gpu.h
 * documents both as SDL-frontend-only. gpu_pick() likewise stays
 * ogl_select_modern(), which viewport.c calls directly.
 */

#include "common.h"
#include "gpu.h"

#include <epoxy/gl.h>

#include "ogl.h"
#include "tmaptext.h" /* text_upload_mvp( ) */


FsvGpuMatrices gpu_mat;

/* Pending draw state, flushed into uniforms by gpu_draw( ) */
static GLfloat pending_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
static GLint pending_lighting = 1;


void
gpu_set_color( float r, float g, float b, float a )
{
	pending_color[0] = r;
	pending_color[1] = g;
	pending_color[2] = b;
	pending_color[3] = a;
}


void
gpu_set_lighting( int enabled )
{
	pending_lighting = enabled ? 1 : 0;
}


void
gpu_set_depth_test( FsvDepthTest test )
{
	switch (test) {
		case FSV_DEPTH_LEQUAL:
		glDepthFunc( GL_LEQUAL );
		break;

		case FSV_DEPTH_GREATER:
		glDepthFunc( GL_GREATER );
		break;

		default:
		glDepthFunc( GL_LESS );
		break;
	}
}


void
gpu_set_line_width( float width )
{
	glLineWidth( width );
}


FsvRenderMode
gpu_render_mode( void )
{
	return gl.render_mode == RENDERMODE_SELECT ? FSV_RENDER_SELECT
						   : FSV_RENDER_NORMAL;
}


/* Port of ogl_upload_matrices( ). The old `text` argument is gone: the
 * text engine's MVP is now always refreshed alongside the scene's. Every
 * call site that passed FALSE did so only to skip work, never because a
 * stale text matrix was wanted, and the one place that deliberately sets
 * a different text matrix (about_splash_draw( )'s ortho projection) calls
 * text_upload_mvp( ) directly, after the last gpu_upload_matrices( ). */
void
gpu_upload_matrices( void )
{
	mat4 mvp;
	glm_mat4_mul( gpu_mat.projection, gpu_mat.modelview, mvp );

	mat3 normmat;
	glm_mat4_pick3( gpu_mat.modelview, normmat );
	glm_mat3_inv( normmat, normmat );
	glm_mat3_transpose( normmat );

	glUseProgram( gl.program );
	glUniformMatrix4fv( gl.modelview_location, 1, GL_FALSE, (float *)gpu_mat.modelview );
	glUniformMatrix3fv( gl.normal_matrix_location, 1, GL_FALSE, (float *)normmat );
	glUniformMatrix4fv( gl.mvp_location, 1, GL_FALSE, (float *)mvp );
	glUseProgram( 0 );

	text_upload_mvp( (float *)mvp );
}


/* GL primitive mode for a topology. Unlike SDL_GPU, GL has all six, so
 * nothing has to be converted here. */
static GLenum
gl_mode( FsvTopology topology )
{
	switch (topology) {
		case FSV_TRIANGLES:
		return GL_TRIANGLES;

		case FSV_TRIANGLE_FAN:
		return GL_TRIANGLE_FAN;

		case FSV_TRIANGLE_STRIP:
		return GL_TRIANGLE_STRIP;

		case FSV_LINES:
		return GL_LINES;

		case FSV_LINE_STRIP:
		return GL_LINE_STRIP;

		case FSV_LINE_LOOP:
		return GL_LINE_LOOP;

		SWITCH_FAIL
	}

	return GL_TRIANGLES;
}


/* Port of the glGenBuffers/glBufferData/glDraw{Arrays,Elements} blocks
 * geometry.c used to inline at every call site (drawVertex( ),
 * drawVertexPos( ), mapv_gldraw_node( ), treev_gldraw_platform( ), ...).
 *
 * One streaming VBO/EBO pair is shared by every call, which is exactly
 * what the original code did -- each call site had its own `static GLuint
 * vbo`, but only ever one buffer alive per site and one draw in flight,
 * so a single shared pair is equivalent and cheaper. */
void
gpu_draw( FsvTopology topology, const FsvVertex *verts, int nverts,
	  const unsigned int *indices, int nindices )
{
	static GLuint vbo, ebo;

	if (verts == NULL || nverts <= 0)
		return;
	if (indices != NULL && nindices <= 0)
		return;

	if (!vbo)
		glGenBuffers( 1, &vbo );
	glBindBuffer( GL_ARRAY_BUFFER, vbo );
	glBufferData( GL_ARRAY_BUFFER, sizeof(FsvVertex) * nverts, verts,
		      GL_STREAM_DRAW );

	glEnableVertexAttribArray( gl.position_location );
	glVertexAttribPointer( gl.position_location, 3, GL_FLOAT, GL_FALSE,
			       sizeof(FsvVertex), (void *)offsetof(FsvVertex, pos) );
	glEnableVertexAttribArray( gl.normal_location );
	glVertexAttribPointer( gl.normal_location, 3, GL_FLOAT, GL_FALSE,
			       sizeof(FsvVertex), (void *)offsetof(FsvVertex, normal) );

	glUseProgram( gl.program );
	glUniform4fv( gl.color_location, 1, pending_color );
	glUniform1i( gl.lightning_enabled_location, pending_lighting );

	if (indices != NULL) {
		if (!ebo)
			glGenBuffers( 1, &ebo );
		glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, ebo );
		glBufferData( GL_ELEMENT_ARRAY_BUFFER,
			      sizeof(unsigned int) * nindices, indices,
			      GL_STREAM_DRAW );
		glDrawElements( gl_mode( topology ), nindices, GL_UNSIGNED_INT, 0 );
		glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
	}
	else
		glDrawArrays( gl_mode( topology ), 0, nverts );

	glUseProgram( 0 );

	/* Avoid implicit sync by allowing GL to dealloc memory */
	glBufferData( GL_ARRAY_BUFFER, sizeof(FsvVertex) * nverts, NULL,
		      GL_STREAM_DRAW );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
}


/* end ogl-gpu-compat.c */
