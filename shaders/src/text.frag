// SPDX-License-Identifier: MIT
//
// Ported from src/fsv-text-fragment.glsl (OpenGL 3.1 / GLSL 140) to
// Vulkan-flavored GLSL 4.50 for the SDL_GPU (Metal on macOS) renderer.
// See shaders/src/scene.vert for the full SDL_GPU binding-order contract.
//
//   Fragment shaders -> uniform buffers at set=3; combined image-samplers
//                        at set=2.
//
// --- Uniform inventory (derived from src/tmaptext.c) ---------------------
//   color : glUniform3f(glt.color_location, ...) — per-label text tint,
//           set before drawing each text run -> fragment UBO (set=3,
//           binding=0).
//   tex   : glUniform1i(glt.texture_location, 0) — the glyph atlas, bound
//           once to texture unit 0 at startup -> combined sampler
//           (set=2, binding=0).

#version 450

layout(location = 0) in vec2 Texcoord;

layout(set = 2, binding = 0) uniform sampler2D tex;

layout(set = 3, binding = 0) uniform TextFragUBO {
    vec3 color;
} u;

layout(location = 0) out vec4 outputColor;

void main() {
    vec4 alpha = texture(tex, Texcoord);
    outputColor = vec4(u.color, alpha.r);
}
