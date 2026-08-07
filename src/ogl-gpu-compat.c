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
#include <gio/gio.h> /* g_resources_lookup_data( ), text shader source */

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


/* ---- Texture-mapped text (src/gpu.h's text slice, src/tmaptext.c) ----
 *
 * tmaptext.c used to own its own GL program (`glt`) and texture object
 * (`text_tobj`) directly -- a second, independent GL program alongside
 * gl.program, loaded from the same still-unported GLSL 140 resources
 * (/jabl/fsv/fsv-text-{vertex,fragment}.glsl) as before Task 3.1, since
 * that port only touched shaders/src/text.{vert,frag} (the SDL_GPU/MSL
 * side). Moving here is purely mechanical: same program, same texture
 * parameters, same draw -- just relocated behind gpu_text_*( ) so
 * tmaptext.c itself no longer contains GL.
 */

static struct {
	GLuint program;
	GLint mvp_location;
	GLint position_location;
	GLint texcoord_location;
	GLint texture_location;
	GLint color_location;
} glt;

static GLuint text_tobj;


/* Port of tmaptext.c's old text_init_shaders( ). */
static GLuint
text_init_shaders( void )
{
	GBytes *source;
	GLuint program = 0, vertex = 0, fragment = 0;

	source = g_resources_lookup_data("/jabl/fsv/fsv-text-vertex.glsl", 0, NULL);
	vertex = ogl_create_shader(GL_VERTEX_SHADER, g_bytes_get_data(source, NULL));
	g_bytes_unref(source);
	if (vertex == 0)
		goto out;

	source = g_resources_lookup_data("/jabl/fsv/fsv-text-fragment.glsl", 0, NULL);
	fragment = ogl_create_shader(GL_FRAGMENT_SHADER, g_bytes_get_data(source, NULL));
	g_bytes_unref(source);
	if (fragment == 0)
		goto out;

	program = glCreateProgram();
	glAttachShader(program, vertex);
	glAttachShader(program, fragment);
	glLinkProgram(program);

	GLint status = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &status);
	if (status == GL_FALSE) {
		GLint log_len = 0;
		glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_len);
		char *buffer = g_malloc(log_len + 1);
		glGetProgramInfoLog(program, log_len, NULL, buffer);
		g_error("Linking failure in text program: %s", buffer);
		g_free(buffer);
		glDeleteProgram(program);
		program = 0;
		goto out;
	}

	glt.mvp_location = glGetUniformLocation(program, "mvp");
	glt.color_location = glGetUniformLocation(program, "color");
	glt.texture_location = glGetUniformLocation(program, "tex");
	glt.position_location = glGetAttribLocation(program, "position");
	glt.texcoord_location = glGetAttribLocation(program, "texcoord");

	glDetachShader(program, vertex);
	glDetachShader(program, fragment);

out:
	if (vertex != 0)
		glDeleteShader(vertex);
	if (fragment != 0)
		glDeleteShader(fragment);

	return program;
}


/* Port of tmaptext.c's old text_init( )'s texture half. */
void
gpu_text_init( const unsigned char *pixels, int width, int height )
{
	float border_color[] = { 0.0, 0.0, 0.0, 1.0 };

	glGenTextures( 1, &text_tobj );
	glBindTexture( GL_TEXTURE_2D, text_tobj );

	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameterfv( GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border_color );

	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	/* GL_RED is the only single-channel format modern GL guarantees; the
	 * fragment shader swizzles it into the output alpha. */
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RED, width, height, 0, GL_RED,
		      GL_UNSIGNED_BYTE, pixels );
	glGenerateMipmap( GL_TEXTURE_2D );
	glBindTexture( GL_TEXTURE_2D, 0 );

	glt.program = text_init_shaders( );
	if (!glt.program)
		g_error( "Compiling text shaders failed" );
}


/* Port of tmaptext.c's old text_pre( ) / text_post( ). */
void
gpu_text_begin( void )
{
	glDisable( GL_POLYGON_OFFSET_FILL );
	glEnable( GL_BLEND );
	glBindTexture( GL_TEXTURE_2D, text_tobj );
}


void
gpu_text_end( void )
{
	glDisable( GL_BLEND );
	glEnable( GL_POLYGON_OFFSET_FILL );
	glBindTexture( GL_TEXTURE_2D, 0 );
}


/* Port of tmaptext.c's old draw_text_vertices( ). One streaming VBO/EBO
 * pair, same shape as gpu_draw( )'s -- see the note at the top of this
 * file about why indices are re-uploaded rather than cached. */
void
gpu_text_draw( const FsvTextVertex *verts, int nverts,
	       const unsigned int *indices, int nindices )
{
	static GLuint vbo, ebo;

	if (verts == NULL || nverts <= 0 || indices == NULL || nindices <= 0)
		return;

	if (!vbo)
		glGenBuffers( 1, &vbo );
	glBindBuffer( GL_ARRAY_BUFFER, vbo );
	glBufferData( GL_ARRAY_BUFFER, sizeof(FsvTextVertex) * nverts, verts,
		      GL_STREAM_DRAW );

	glEnableVertexAttribArray( glt.position_location );
	glVertexAttribPointer( glt.position_location, 3, GL_FLOAT, GL_FALSE,
			       sizeof(FsvTextVertex), (void *)offsetof(FsvTextVertex, pos) );
	glEnableVertexAttribArray( glt.texcoord_location );
	glVertexAttribPointer( glt.texcoord_location, 2, GL_FLOAT, GL_FALSE,
			       sizeof(FsvTextVertex), (void *)offsetof(FsvTextVertex, texcoord) );

	if (!ebo)
		glGenBuffers( 1, &ebo );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, ebo );
	glBufferData( GL_ELEMENT_ARRAY_BUFFER, sizeof(unsigned int) * nindices,
		      indices, GL_STREAM_DRAW );

	glUseProgram( glt.program );
	glUniform1i( glt.texture_location, 0 );
	glDrawElements( GL_TRIANGLES, nindices, GL_UNSIGNED_INT, 0 );
	glUseProgram( 0 );

	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
	/* Avoid implicit sync by allowing GL to dealloc memory */
	glBufferData( GL_ARRAY_BUFFER, sizeof(FsvTextVertex) * nverts, NULL,
		      GL_STREAM_DRAW );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
}


void
gpu_text_set_color( float r, float g, float b )
{
	glUseProgram( glt.program );
	glUniform3f( glt.color_location, r, g, b );
	glUseProgram( 0 );
}


void
gpu_text_upload_mvp( const float *mvp )
{
	glUseProgram( glt.program );
	glUniformMatrix4fv( glt.mvp_location, 1, GL_FALSE, mvp );
	glUseProgram( 0 );
}


/* end ogl-gpu-compat.c */
