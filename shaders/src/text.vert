// SPDX-License-Identifier: MIT
//
// Ported from src/fsv-text-vertex.glsl (OpenGL 3.1 / GLSL 140) to
// Vulkan-flavored GLSL 4.50 for the SDL_GPU (Metal on macOS) renderer.
// See shaders/src/scene.vert for the full SDL_GPU binding-order contract.
//
//   Vertex shaders -> uniform buffers at set=1 (binding 0, ...); no
//                      vertex-stage samplers used by this shader.
//
// --- Uniform inventory (derived from src/tmaptext.c) ---------------------
//   mvp : glUniformMatrix4fv(glt.mvp_location, ...) via text_upload_mvp(),
//         called once per frame right after ogl_upload_matrices() — the
//         only vertex-stage uniform text glyphs need.

#version 450

layout(set = 1, binding = 0) uniform TextVertUBO {
    mat4 mvp;
} u;

layout(location = 0) in vec3 position;
layout(location = 1) in vec2 texcoord;

layout(location = 0) out vec2 Texcoord;

void main() {
    gl_Position = u.mvp * vec4(position, 1.0);
    Texcoord = texcoord;
}
