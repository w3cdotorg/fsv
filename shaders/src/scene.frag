// SPDX-License-Identifier: MIT
//
// Ported from src/fsv-fragment.glsl (OpenGL 3.1 / GLSL 140) to
// Vulkan-flavored GLSL 4.50 for the SDL_GPU (Metal on macOS) renderer.
// See shaders/src/scene.vert for the full SDL_GPU binding-order contract.
//
//   Fragment shaders -> uniform buffers at set=3; combined image-samplers
//                        at set=2.
//   SDL_PushGPUFragmentUniformData(cmd, /*slot*/ 0, &ubo, sizeof ubo) feeds
//   set=3 binding=0 below. No samplers here: the 3-D scene geometry is
//   untextured (unlike text.frag).
//
// --- Uniform inventory (derived from src/geometry.c / src/ogl.c) --------
//   color             : glUniform4f(gl.color_location, ...) — set on
//                        nearly every draw call (src/geometry.c).
//   ambient/diffuse/
//   specular          : glUniform1f(...), set once at light setup
//                        (src/ogl.c) and constant thereafter.
//   lightning_enabled : glUniform1i(gl.lightning_enabled_location, ...) —
//                        toggled per-draw-call; duplicated from the vertex
//                        UBO for the reason documented in scene.vert.

#version 450

layout(location = 0) in vec3 fragPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec4 lightPos;

layout(location = 0) out vec4 outputColor;

layout(set = 3, binding = 0) uniform SceneFragUBO {
    vec4  color;
    float ambient;
    float diffuse;
    float specular;
    int   lightning_enabled;
} u;

void main() {
    if (u.lightning_enabled == 0) {
        outputColor = u.color;
        return;
    }

    vec3 light_color = vec3(1.0, 1.0, 1.0);
    // Ambient light
    vec3 ambient_light = u.ambient * light_color;

    // Diffuse light
    vec3 lightDir;
    if (lightPos.w == 0.0)  // Light at infinity
        lightDir = normalize(lightPos.xyz);
    else
        lightDir = normalize(lightPos.xyz - fragPos);
    vec3 fragNN = normalize(fragNormal);
    float diffuse_refl = max(dot(fragNN, lightDir), 0.0);
    vec3 diffuse_light = diffuse_refl * u.diffuse * light_color;

    // Specular light
    vec3 viewPos = vec3(0.0, 0.0, 0.0);
    vec3 viewDir = normalize(viewPos - fragPos);
    vec3 reflectDir = reflect(-lightDir, fragNN);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), 2);
    vec3 spec_light = u.specular * spec * light_color;

    // Final color from lightning calculation
    outputColor = vec4(((ambient_light + diffuse_light + spec_light) * u.color.rgb), u.color.a);

    // NOTE: the original src/fsv-fragment.glsl has a commented-out debug
    // line here (force-visualize fragNormal). It was inert in the original
    // (never compiled) and is intentionally not carried over — no lighting
    // math is dropped, only dead debug scaffolding.
}
