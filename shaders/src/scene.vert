// SPDX-License-Identifier: MIT
//
// Ported from src/fsv-vertex.glsl (OpenGL 3.1 / GLSL 140) to
// Vulkan-flavored GLSL 4.50 for the SDL_GPU (Metal on macOS) renderer.
//
// --- SDL_GPU shader resource contract (CONTRACT for Task 3.2) ---------
//   Vertex shaders   -> uniform buffers at set=1 (binding 0, 1, ...);
//                        samplers/textures at set=0.
//   Fragment shaders -> uniform buffers at set=3; combined image-samplers
//                        at set=2.
//   SDL_PushGPUVertexUniformData(cmd, /*slot*/ 0, &ubo, sizeof ubo) feeds
//   set=1 binding=0 below.
//
//   std140 note: mat3 does NOT pack tightly in std140 (each column pads to
//   a vec4, and the *host* struct must mirror that padding exactly or the
//   layout silently corrupts). To sidestep the mismatch entirely,
//   `normal_matrix` is declared here as a full mat4 (upper-left 3x3 is the
//   real normal matrix; the last row/column are inert padding) and
//   truncated with mat3(...) below. The C++ UBO struct in gpu.h (Task 3.2)
//   must mirror this mat4, not mat3.
//
// --- Uniform inventory (derived from src/ogl.c / src/geometry.c, NOT ----
// --- from the task brief's sketch — see task-3.1-report.md for the diff)
//   mvp, modelview, normal_matrix : uploaded once per frame in
//                                    ogl_upload_matrices() (src/ogl.c).
//   light_pos                     : uploaded once at light setup
//                                    (src/ogl.c, glUniform4fv on
//                                    gl.light_pos_location) — the brief's
//                                    sketch omitted this uniform from the
//                                    vertex-stage UBO even though the
//                                    original vertex shader consumes it.
//   lightning_enabled              : toggled per-draw-call, many times per
//                                    frame (src/geometry.c,
//                                    ogl_enable_lightning/disable_lightning).
//                                    Also required by scene.frag — desktop
//                                    GLSL shared one uniform namespace
//                                    across both stages of a program;
//                                    SDL_GPU's per-stage UBOs do not, so
//                                    this value is duplicated into both
//                                    the vertex and fragment UBOs.
//
// All five values are bundled into one per-draw UBO; the host code
// re-pushes it before every draw call regardless of which values actually
// changed since the last draw.

#version 450

layout(set = 1, binding = 0) uniform SceneVertUBO {
    mat4 mvp;
    mat4 modelview;
    mat4 normal_matrix; // upper-left 3x3 valid; see std140 note above
    vec4 light_pos;
    int  lightning_enabled;
} u;

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;

layout(location = 0) out vec3 fragPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec4 lightPos;

void main() {
    vec4 pos = vec4(position, 1.0);
    gl_Position = u.mvp * pos;

    if (u.lightning_enabled != 0) {
        lightPos = u.modelview * u.light_pos;

        // Position of vertex in camera coordinates interpolated to frag coords
        vec4 fragTmp = u.modelview * pos;
        fragPos = fragTmp.xyz / fragTmp.w;

        fragNormal = mat3(u.normal_matrix) * normal;
    }
}
