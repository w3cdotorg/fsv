#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Builds docs/media/demo.mp4 + docs/media/demo.gif: a ~24s demo video of
# fsv navigating this repository's own src/ tree in FSN mode, closing on
# the squarified MapV.
#
# Pipeline:
#   1. Build the SDL/Metal frontend (builddir, meson's default target
#      name -- see README.md/docs/PORTING.md) if it doesn't exist yet.
#   2. `fsv --fsn --record`: a scripted camera flythrough of src/,
#      starting in FSN mode, rendered offscreen (scene + ImGui
#      composited, see src/sdl/gpu.cpp's "--record" section) into
#      numbered BMP frames in a scratch dir.
#   3. ffmpeg: BMPs -> demo.mp4 (h264, yuv420p -- the widely-compatible
#      combination GitHub/browsers expect), then demo.mp4 -> demo.gif
#      (palette-optimized two-pass, downscaled + reduced fps for size).
#   4. Delete the frame dump (disk is cheap; the repo isn't).
#
# Usage: tools/make-demo.sh [duration_seconds]
#   duration_seconds defaults to 24 (Task 6.4's ≤20s budget is superseded
#   now that the script covers FSN + the MapV finale; the GIF's ≤10MB
#   budget and the retry-smaller logic below are unchanged).

set -euo pipefail

cd "$(dirname "$0")/.."

DURATION="${1:-24}"
FSV_BIN="${FSV_BIN:-builddir/src/sdl/fsv}"
OUT_DIR="docs/media"
MP4="$OUT_DIR/demo.mp4"
GIF="$OUT_DIR/demo.gif"

for cmd in ffmpeg ffprobe; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "error: $cmd not found (brew install ffmpeg)" >&2
        exit 1
    fi
done

if [ ! -x "$FSV_BIN" ]; then
    echo "==> $FSV_BIN not found; building it"
    if [ ! -d builddir ]; then
        meson setup builddir -Dfrontend=sdl
    fi
    ninja -C builddir
fi
if [ ! -x "$FSV_BIN" ]; then
    echo "error: $FSV_BIN still missing after build -- pass FSV_BIN=path" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"

FRAMES_DIR="$(mktemp -d "${TMPDIR:-/tmp}/fsv-demo-frames.XXXXXX")"
PALETTE="$(mktemp "${TMPDIR:-/tmp}/fsv-demo-palette.XXXXXX.png")"
trap 'rm -rf "$FRAMES_DIR" "$PALETTE"' EXIT

echo "==> recording ${DURATION}s of src/ navigation into $FRAMES_DIR"
"$FSV_BIN" src --fsn --record "$FRAMES_DIR" "$DURATION"

echo "==> encoding $MP4"
ffmpeg -y -loglevel error -framerate 30 -i "$FRAMES_DIR/frame_%05d.bmp" \
    -c:v libx264 -pix_fmt yuv420p -movflags +faststart "$MP4"

# Palette-optimized two-pass GIF (ffmpeg's own documented approach for a
# quality/size tradeoff): downscaled to 720px wide and dropped to 15fps
# -- a demo GIF doesn't need the mp4's full resolution/framerate, and
# both cuts matter for staying under the ~10MB budget GitHub-rendered
# READMEs are comfortable with.
echo "==> building palette"
ffmpeg -y -loglevel error -i "$MP4" \
    -vf "fps=15,scale=720:-1:flags=lanczos,palettegen" "$PALETTE"

echo "==> encoding $GIF"
ffmpeg -y -loglevel error -i "$MP4" -i "$PALETTE" \
    -lavfi "fps=15,scale=720:-1:flags=lanczos [x]; [x][1:v] paletteuse" \
    "$GIF"

gif_bytes=$(wc -c <"$GIF" | tr -d ' ')
if [ "$gif_bytes" -gt 10485760 ]; then
    echo "==> $GIF is $((gif_bytes / 1048576))MB (>10MB budget); retrying smaller (480px/10fps)"
    ffmpeg -y -loglevel error -i "$MP4" \
        -vf "fps=10,scale=480:-1:flags=lanczos,palettegen" "$PALETTE"
    ffmpeg -y -loglevel error -i "$MP4" -i "$PALETTE" \
        -lavfi "fps=10,scale=480:-1:flags=lanczos [x]; [x][1:v] paletteuse" \
        "$GIF"
fi

echo "==> done"
ffprobe -v error -show_entries format=duration,size -of default=noprint_wrappers=1 "$MP4"
ls -lh "$MP4" "$GIF"
