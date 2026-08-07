/* about.c */

/* Help -> About... */

/* fsv - 3D File System Visualizer
 * Copyright (C)1999 Daniel Richard G. <skunk@mit.edu>
 * Copyright (C) 2021 Janne Blomqvist <blomqvist.janne@gmail.com>
 *
 * SPDX-License-Identifier:  LGPL-2.1-or-later
 */


#include "common.h"
#include "about.h"

#include <epoxy/gl.h>

#include "animation.h"
#include "geometry.h"
#include "ogl.h"

/* 3D geometry for the "fsv" logo */
#include "fsv3d.h"
#include "tmaptext.h"


/* Interval normalization macro */
#define INTERVAL_PART(x,x0,x1)	(((x) - (x0)) / ((x1) - (x0)))


/* Normalized time variable (in range [0, 1]) */
static double about_part;

/* TRUE while giving About presentation */
static boolean about_active = FALSE;


/* Draws "fsv" in 3D.
 * Lives here rather than in geometry.c because it is the only geometry
 * drawn through the separate "about" shader program (per-vertex color +
 * linear fog, src/fsv-about-*.glsl) -- see the note in about.h. */
static void
about_gldraw_fsv( void )
{
	XYvec p, n;
	const float *vertices = NULL;
	const int *triangles = NULL, *edges = NULL;
	int v, e, i;
	// Magic constants 490 and 1188 determined by first making these arrays
	// larger and checking vlen and ilen values in the debugger.
#define VERT_MAX_LEN 490
#define IDX_MAX_LEN 1188
	static AboutVertex vert[VERT_MAX_LEN];
	static GLushort idx[IDX_MAX_LEN];
	static GLuint vbo, ebo;
	static size_t vlen;
	static size_t ilen;

	if (!vbo) {
		for (size_t c = 0; c < 3; c++) {
			GLfloat *color = (GLfloat*)&fsv_colors[c];
			vertices = fsv_vertices[c];
			triangles = fsv_triangles[c];
			edges = fsv_edges[c];
			const EdgeSmoothness *es = fsv_edge_smoothness[c];
			// Side faces
			for (e = 0; edges[e] >= 0; e++) {
				// For a smooth edge, calculate the normal from
				// the previous and next vertices. For a sharp
				// edge, duplicate the vertices, normal for the
				// first is calculated from the previous and
				// current vertex, and for the second vertex
				// the normal is calculated from the current
				// and next vertex.
				// Edge here meaning the vertex indices of the
				// vertices forming the sides of the character,
				// not the "edge" from graph theory.
				i = edges[e];
				EdgeSmoothness s = es[e];
				p.x = vertices[2 * i];
				p.y = vertices[2 * i + 1];
				int inext = edges[e + 1];
				int iprev = edges[e - 1];
				XYvec n2;
				if (e == 0) {
					// First edge, must use only "forward"
					// delta
					s = SMOOTH;
					i = inext;
					n.x = vertices[2 * i + 1] - p.y;
					n.y = p.x - vertices[2 * i];
				} else if (inext < 0) {
					// Last edge, must use only "backward"
					// delta
					s = SMOOTH;
					n.x = p.y - vertices[2 * iprev + 1];
					n.y = vertices[2 * iprev] - p.x;
				} else if (s == SMOOTH) {
					i = inext;
					n.x = vertices[2 * i + 1] -
					      vertices[2 * iprev + 1];
					n.y = vertices[2 * iprev] -
					      vertices[2 * i];
				} else if (s == SHARP) {
					// First normal with backward delta.
					n.x = p.y - vertices[2 * iprev + 1];
					n.y = vertices[2 * iprev] - p.x;
					// Second normal with forward delta.
					i = inext;
					n2.x = vertices[2 * i + 1] - p.y;
					n2.y = p.x - vertices[2 * i];
				} else
					g_error("Unable to calculate normal!");

				if (e > 0) {
					idx[ilen++] = vlen - 2;
					idx[ilen++] = vlen - 1;
					idx[ilen++] = vlen;
					idx[ilen++] = vlen;
					idx[ilen++] = vlen - 1;
					idx[ilen++] = vlen + 1;
				}
				vert[vlen++] = (AboutVertex){
				    {p.x, p.y, 30.0},
				    {n.x, n.y, 0.0f},
				    {color[0], color[1], color[2]}};
				vert[vlen++] = (AboutVertex){
				    {p.x, p.y, -30.0},
				    {n.x, n.y, 0.0f},
				    {color[0], color[1], color[2]}};
				if (s == SHARP) {
					// Second set of vertices with forward
					// delta normals.
					vert[vlen++] = (AboutVertex){
					    {p.x, p.y, 30.0},
					    {n2.x, n2.y, 0.0f},
					    {color[0], color[1], color[2]}};
					vert[vlen++] = (AboutVertex){
					    {p.x, p.y, -30.0},
					    {n2.x, n2.y, 0.0f},
					    {color[0], color[1], color[2]}};
				}
			}
			/* Front faces */
			int imax = 0;
			for (v = 0; triangles[v] >= 0; v++) {
				i = triangles[v];
				imax = MAX(i, imax);
				p.x = vertices[2 * i];
				p.y = vertices[2 * i + 1];
				vert[vlen + i] = (AboutVertex){
					{p.x, p.y, 30.0},
					{0.0f, 0.0f, 1.0f},
					{color[0], color[1], color[2]}
				};
				idx[ilen++] = vlen + i;
			}
			vlen += imax + 1;

			/* Back faces */
			imax = 0;
			for (--v; v >= 0; v--) {
				i = triangles[v];
				imax = MAX(i, imax);
				p.x = vertices[2 * i];
				p.y = vertices[2 * i + 1];
				vert[vlen + i] = (AboutVertex){
					{p.x, p.y, -30.0},
					{0.0f, 0.0f, -1.0f},
					{color[0], color[1], color[2]}
				};
				idx[ilen++] = vlen + i;
			}
			vlen += imax + 1;
		}
		g_assert(VERT_MAX_LEN >= vlen);
		g_assert(IDX_MAX_LEN >= ilen);
#undef VERT_MAX_LEN
#undef IDX_MAX_LEN
		glGenBuffers(1, &vbo);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(AboutVertex) * vlen, vert, GL_STATIC_DRAW);

		glGenBuffers(1, &ebo);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(GLushort) * ilen, idx, GL_STATIC_DRAW);

		// Upload fog parameters. Emulate legacy GL with a simple
		// linear fog model.
		glUseProgram(aboutGL.program);
		glUniform3f(aboutGL.fog_color_location, 0.0f, 0.0f, 0.0f);
		glUniform1f(aboutGL.fog_start_location, 200.0f);
		glUniform1f(aboutGL.fog_end_location, 1800.0f);

		ogl_error();
	}
#if 0
	// Useful code snippet for checking vertex coords in NDC
	for (size_t i = 0; i < vlen; i += (vlen - 1)) {
		g_print("%zu th vertex in FSV coords:\n", i);
		vec4 c = (vec4){vert[i].position[0], vert[i].position[1],
				vert[i].position[2], 1.0f};
		vec4 cp;
		glm_mat4_mulv(aboutGL.mvp, c, cp);
		glmc_vec4_print(c, stdout);
		glmc_vec4_print(cp, stdout);
		float w = cp[3];
		vec3 pd = (vec3){cp[0]/w, cp[1]/w, cp[2]/w};
		glmc_vec3_print(pd, stdout);
	}
	glmc_mat4_print(aboutGL.mvp, stdout);
#endif
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glEnableVertexAttribArray(aboutGL.position_location);
	glVertexAttribPointer(aboutGL.position_location, 3, GL_FLOAT, GL_FALSE,
			      sizeof(AboutVertex),
			      (void *)offsetof(AboutVertex, position));
	glEnableVertexAttribArray(aboutGL.normal_location);
	glVertexAttribPointer(aboutGL.normal_location, 3, GL_FLOAT, GL_FALSE,
			      sizeof(AboutVertex),
			      (void *)offsetof(AboutVertex, normal));
	glEnableVertexAttribArray(aboutGL.color_location);
	glVertexAttribPointer(aboutGL.color_location, 3, GL_FLOAT, GL_FALSE,
			      sizeof(AboutVertex),
			      (void *)offsetof(AboutVertex, color));
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
	glUseProgram(aboutGL.program);
	glDrawElements(GL_TRIANGLES, ilen, GL_UNSIGNED_SHORT, 0);
	glUseProgram(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	ogl_error();
}


/* Draws the splash screen */
void
about_splash_draw( void )
{
	XYZvec text_pos;
	XYvec text_dims;
	double bottom_y;
	double k;

	/* Draw fsv title */

	/* Set up projection matrix */
	k = 82.84 / ogl_aspect_ratio( );
	mat4 proj;
	glm_frustum(-70.82, 95.40, - k, k, 200.0, 400.0, proj);

	/* Set up modelview matrix */
	mat4 mv;
	glm_mat4_identity(mv);
	glm_translate(mv, (vec3){0.0, 0.0, -300.0});
	glm_rotate_x(mv, 10.5 * M_PI/180.0, mv);
	glm_translate(mv, (vec3){20.0, 20.0, -30.0});

	mat4 mvp;
	glm_mat4_mul(proj, mv, mvp);
	mat3 normmat;
	glm_mat4_pick3(mv, normmat);
	glm_mat3_inv(normmat, normmat);
	glm_mat3_transpose(normmat);
	glUseProgram(aboutGL.program);
	/* update the "mvp" matrix we use in the shader */
	glUniformMatrix4fv(aboutGL.mvp_location, 1, GL_FALSE, (float*)mvp);
	glUniformMatrix4fv(aboutGL.modelview_location, 1, GL_FALSE, (float*) mv);
	glUniformMatrix3fv(aboutGL.normal_matrix_location, 1, GL_FALSE, (float*) normmat);
	glUseProgram(0);

	about_gldraw_fsv( );

	/* Draw accompanying text */

	/* Set up projection matrix */
	k = 0.5 / ogl_aspect_ratio( );
	// Reuse proj matrix from the "FSV" drawing
	glm_ortho(0.0, 1.0, - k, k, -1.0, 1.0, proj);
	bottom_y = - k;

	/* Set up modelview matrix */
	// Modelview is the identity, so mvp is just the projection matrix.
	text_upload_mvp((float*) proj);

	text_pre( );

	/* Title */
	text_set_color(1.0, 1.0, 1.0);
	text_pos.x = 0.2059;
	text_pos.y = -0.1700;
	text_pos.z = 0.0;
	text_dims.x = 0.9;
	text_dims.y = 0.0625;
	text_draw_straight( "File", &text_pos, &text_dims );
	text_pos.x = 0.4449;
	text_draw_straight( "System", &text_pos, &text_dims );
	text_pos.x = 0.7456;
	text_draw_straight( "Visualizer", &text_pos, &text_dims );

	/* Version */
	text_set_color(0.75, 0.75, 0.75);
	text_pos.x = 0.5000;
	text_pos.y = (2.0 - MAGIC_NUMBER) * (0.2247 + bottom_y) - 0.2013;
	text_dims.y = 0.0386;
	text_draw_straight( "Version " VERSION, &text_pos, &text_dims );

	/* Copyright/author info */
	text_set_color(0.5, 0.5, 0.5);
	text_pos.y = bottom_y + 0.0417;
	text_dims.y = 0.0234;
	text_draw_straight( "Copyright (C)1999 Daniel Richard G. <skunk@mit.edu>", &text_pos, &text_dims );
	text_pos.y = bottom_y + 0.0117;
	text_draw_straight("Copyright (C) 2021 Janne Blomqvist", &text_pos, &text_dims);

	text_post( );
}


/* Draws the "fsv" 3D letters */
static void
draw_fsv( void )
{
	double dy, p, q;

	/* Set up projection matrix */
	dy = 80.0 / ogl_aspect_ratio( );
	mat4 proj;
	glm_frustum(-80.0, 80.0, -dy, dy, 80.0, 2000.0, proj);

	/* Set up modelview matrix */
	mat4 mv;
	glm_mat4_identity(mv);
	if (about_part < 0.5) {
		/* Spinning and approaching fast */
		p = INTERVAL_PART(about_part, 0.0, 0.5);
		q = pow( 1.0 - p, 1.5 );
		glm_translate(mv, (vec3){0.0, 0.0, -150.0 - 1800.0 * q});
		glm_rotate_y(mv, 900.0 * q * M_PI/180., mv);
	}
	else if (about_part < 0.625) {
		/* Holding still for a moment */
		glm_translate(mv, (vec3){0.0, 0.0, -150.0});
	}
	else if (about_part < 0.75) {
		/* Flipping up and back */
		p = INTERVAL_PART(about_part, 0.625, 0.75);
		q = 1.0 - SQR(1.0 - p);
		glm_translate(mv, (vec3){0.0, 40.0 * q, -150.0 - 50.0 * q});
		glm_rotate_x(mv, 365.0 * q * M_PI/180., mv);
	}
	else {
		/* Holding still again */
		glm_translate(mv, (vec3){0.0, 40.0, -200.0});
		glm_rotate_x(mv, 5.0 * M_PI/180.0, mv);
	}

	mat4 mvp;
	glm_mat4_mul(proj, mv, mvp);
	mat3 normmat;
	glm_mat4_pick3(mv, normmat);
	glm_mat3_inv(normmat, normmat);
	glm_mat3_transpose(normmat);
	glUseProgram(aboutGL.program);
	/* update the "mvp" matrix we use in the shader */
	glUniformMatrix4fv(aboutGL.mvp_location, 1, GL_FALSE, (float*)mvp);
	glUniformMatrix4fv(aboutGL.modelview_location, 1, GL_FALSE, (float*) mv);
	glUniformMatrix3fv(aboutGL.normal_matrix_location, 1, GL_FALSE, (float*) normmat);
	glUseProgram(0);

	/* Draw "fsv" geometry */
	about_gldraw_fsv( );
}


/* Draws the lines of text */
static void
draw_text( void )
{
        XYZvec tpos;
	XYvec tdims;
	double dy, p, q;

	if (about_part < 0.625)
		return;

	/* Set up projection matrix */
	dy = 1.0 / ogl_aspect_ratio( );
	mat4 proj;
	glm_frustum(-1.0, 1.0, -dy, dy, 1.0, 205.0, proj);

	/* Set up modelview matrix */
	// Modelview matrix is the identity, so mvp is just proj.
	text_upload_mvp((float*) proj);

        if (about_part < 0.75)
		p = INTERVAL_PART(about_part, 0.625, 0.75);
	else
		p = 1.0;
	q = (1.0 - SQR(1.0 - p));

	text_pre( );

	tdims.x = 400.0;
	tdims.y = 18.0;
	tpos.x = 0.0;
	tpos.y = -35.0; /* -35 */
	tpos.z = -200.0 * q;
	text_set_color(1.0, 1.0, 1.0);
	text_draw_straight( "fsv - 3D File System Visualizer", &tpos, &tdims );

	tdims.y = 15.0;
	tpos.y = 40.0 * q - 95.0; /* -55 */
	text_draw_straight( "Version " VERSION, &tpos, &tdims );

	tdims.y = 12.0;
	tpos.y = 100.0 * q - 180.0; /* -80 */
	text_set_color(0.5, 0.5, 0.5);
	text_draw_straight( "Copyright (C)1999 by Daniel Richard G.", &tpos, &tdims );

	tpos.y = 140.0 * q - 235.0; /* -95 */
	text_draw_straight( "Copyright (C) 2022 Janne Blomqvist", &tpos, &tdims );

	/* Finally, fade in the home page URL */
	if (about_part > 0.75) {
		tpos.y = -115.0;
		p = INTERVAL_PART(about_part, 0.75, 1.0);
		q = SQR(SQR(p));
		text_set_color(q, q, 0.0);
		text_draw_straight( "https://github.com/jabl/fsv/", &tpos, &tdims );
		//text_draw_straight( "__________________________________", &tpos, &tdims );
	}

	text_post( );
}


/* Progress callback; keeps viewport updated during presentation */
static void
about_progress_cb( Morph *unused )
{
	globals.need_redraw = TRUE;
}


/* Control routine */
boolean
about( AboutMesg mesg )
{
	switch (mesg) {
		case ABOUT_BEGIN:
		/* Begin the presentation */
		morph_break( &about_part );
		about_part = 0.0;
		morph_full( &about_part, MORPH_LINEAR, 1.0, 8.0, about_progress_cb, about_progress_cb, NULL );
		about_active = TRUE;
		break;

		case ABOUT_END:
		if (!about_active)
			return FALSE;
		/* We now return you to your regularly scheduled program */
		morph_break( &about_part );
		redraw( );
		about_active = FALSE;
		return TRUE;

		case ABOUT_DRAW:
		/* Draw all presentation elements */
		draw_fsv( );
		draw_text( );
		break;

		case ABOUT_CHECK:
		/* Return current presentation status */
		return about_active;

		SWITCH_FAIL
	}

	return FALSE;
}


/* end about.c */
