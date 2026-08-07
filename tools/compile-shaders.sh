#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Regenerates shaders/compiled/ from shaders/src/*.vert|*.frag.
#
# Toolchain (see docs/PORTING.md "Task 3.1" section for the full rationale):
#   `sdl3_shadercross` / `shadercross` (SDL_shadercross CLI) has no Homebrew
#   formula as of this writing (`brew search shadercross` only turns up the
#   unrelated `shaderc`). We use the battle-tested two-step alternative
#   instead:
#     1. glslangValidator -V   : GLSL 4.50 (Vulkan semantics) -> SPIR-V.
#        `brew install glslang`
#     2. spirv-cross --msl     : SPIR-V -> Metal Shading Language source.
#        `brew install spirv-cross`
#   Optional: `brew install spirv-tools` provides `spirv-val` to validate
#   the generated .spv modules (run automatically below if present).
#
# Entry points (Task 3.2 needs these when creating SDL_GPUShader objects):
#   - SPIR-V (.spv) shaders : entry point is "main" (glslangValidator keeps
#     the GLSL entry-point name as-is).
#   - MSL (.msl) shaders    : entry point is "main0". SPIRV-Cross renames
#     the entry point because Metal reserves the identifier "main" for a
#     different purpose; this is SPIRV-Cross's standard, undocumented-flag
#     behavior, not something we opted into.
#
# Output is deterministic: SPIR-V modules carry no build timestamp (only a
# generator magic number), and SPIRV-Cross's MSL text output is a pure
# function of the input SPIR-V. Running this script twice in a row produces
# byte-identical artifacts (verified via `git diff --stat` in task-3.1).

set -euo pipefail

cd "$(dirname "$0")/../shaders"

if ! command -v glslangValidator >/dev/null 2>&1; then
    echo "error: glslangValidator not found (brew install glslang)" >&2
    exit 1
fi
if ! command -v spirv-cross >/dev/null 2>&1; then
    echo "error: spirv-cross not found (brew install spirv-cross)" >&2
    exit 1
fi

mkdir -p compiled

for f in src/*.vert src/*.frag; do
    base=$(basename "$f")
    stage=${base##*.}       # vert | frag
    name=${base%.*}         # scene | text

    spv="compiled/$name.$stage.spv"
    msl="compiled/$name.$stage.msl"

    echo "==> $f"
    glslangValidator -V -S "$stage" "$f" -o "$spv"

    if command -v spirv-val >/dev/null 2>&1; then
        spirv-val "$spv"
    fi

    spirv-cross "$spv" --msl --output "$msl"
done

echo "Done. Artifacts in shaders/compiled/."
