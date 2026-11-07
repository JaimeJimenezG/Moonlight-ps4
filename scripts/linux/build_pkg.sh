#!/usr/bin/env bash
# Configure, build, and package moonlight-ps4 for PS4.
#
# Usage:
#   ./scripts/linux/build_pkg.sh          # build with libSceVideodec2 (HW at runtime via INI)
#   ./scripts/linux/build_pkg.sh --no-hw  # disable compile-time ML_ENABLE_VIDEODEC2 (ignored since 0.6.2)
#   ./scripts/linux/build_pkg.sh --clean  # reconfigure from scratch
#   BUILD_DIR=build-ps4-debug ./scripts/linux/build_pkg.sh
#
# Requirements: source scripts/linux/env.sh (done automatically), PS4 FFmpeg
# at ~/ps4dev/ffmpeg-ps4 (see scripts/linux/build_ffmpeg_ps4.sh).

set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
# shellcheck source=env.sh
source "$REPO_DIR/scripts/linux/env.sh"

BUILD_DIR="${BUILD_DIR:-$REPO_DIR/build-ps4}"
TOOLCHAIN="$REPO_DIR/cmake/openorbis.cmake"
FFMPEG_PREFIX="${FFMPEG_PS4_PREFIX:-$HOME/ps4dev/ffmpeg-ps4}"
VERSION="$(grep '^VERSION=' "$REPO_DIR/scripts/linux/make_pkg.sh" | sed 's/.*"\(.*\)".*/\1/')"

CLEAN=0
ENABLE_HW=1

usage() {
    sed -n '2,10p' "$0"
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage 0 ;;
        --clean) CLEAN=1; shift ;;
        --hw) ENABLE_HW=1; shift ;;
        --no-hw) ENABLE_HW=0; shift ;;
        *) echo "Unknown option: $1" >&2; usage 1 ;;
    esac
done

if [[ ! -f "$TOOLCHAIN" ]]; then
    echo "error: toolchain not found: $TOOLCHAIN" >&2
    exit 1
fi

if [[ ! -f "$FFMPEG_PREFIX/lib/libavcodec.a" ]]; then
    echo "error: PS4 FFmpeg not found at $FFMPEG_PREFIX" >&2
    echo "Run: $REPO_DIR/scripts/linux/build_ffmpeg_ps4.sh" >&2
    exit 1
fi

CMAKE_ARGS=(
    -G Ninja
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN"
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
)

if [[ "$ENABLE_HW" -eq 0 ]]; then
    echo "build: warning --no-hw ignored (HW always compiled since 0.6.2)"
fi
echo "build: ML_ENABLE_VIDEODEC2=1 (prefer_hw in moonlight.ini)"

if [[ "$CLEAN" -eq 1 ]]; then
    echo "build: cleaning $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

echo "build: configure -> $BUILD_DIR"
cmake -B "$BUILD_DIR" "${CMAKE_ARGS[@]}"

echo "build: compile + pkg"
cmake --build "$BUILD_DIR"

PKG="$BUILD_DIR/Moonlight-${VERSION}.pkg"
if [[ ! -f "$PKG" ]]; then
    echo "error: did not produce $PKG" >&2
    exit 1
fi

echo "OK: $PKG"
