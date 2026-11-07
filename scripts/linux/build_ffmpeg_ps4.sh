#!/usr/bin/env bash
# Cross-build FFmpeg (H.264 decoder only) for OpenOrbis.
# Output: ~/ps4dev/ffmpeg-ps4/{lib,include}
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT/scripts/linux/env.sh"

OO="$OO_PS4_TOOLCHAIN"
SRC_DIR="${FFMPEG_SRC_DIR:-$HOME/ps4dev/src}"
PREFIX="${FFMPEG_PS4_PREFIX:-$HOME/ps4dev/ffmpeg-ps4}"
VER="${FFMPEG_VERSION:-n5.1.6}"

mkdir -p "$SRC_DIR" "$PREFIX"
cd "$SRC_DIR"

if [ ! -d "FFmpeg-$VER" ]; then
    curl -L --fail -o "ffmpeg-$VER.tar.gz" \
        "https://github.com/FFmpeg/FFmpeg/archive/refs/tags/${VER}.tar.gz"
    tar -xzf "ffmpeg-$VER.tar.gz"
fi
cd "FFmpeg-$VER"

# ld.lld wrapper (clang -fuse-ld looks for crtbegin/libgcc that OpenOrbis lacks)
install -m 755 "$ROOT/scripts/linux/ps4-ld.sh" /tmp/ps4-ld.sh

./configure \
  --prefix="$PREFIX" \
  --enable-cross-compile \
  --arch=x86_64 \
  --target-os=freebsd \
  --cc=clang --cxx=clang++ \
  --ld=/tmp/ps4-ld.sh \
  --ar=llvm-ar --ranlib=llvm-ranlib --nm=llvm-nm --strip=llvm-strip \
  --extra-cflags="--target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -isysroot ${OO} -isystem ${OO}/include -D__PS4__ -D__ORBIS__ -O2" \
  --disable-shared --enable-static \
  --disable-programs --disable-doc --disable-debug --disable-htmlpages --disable-manpages \
  --disable-avdevice --disable-avformat --disable-avfilter \
  --disable-swresample --disable-swscale --disable-postproc \
  --disable-network --disable-iconv --disable-zlib --disable-bzlib \
  --disable-everything \
  --enable-avcodec --enable-avutil \
  --enable-decoder=h264 --enable-parser=h264 \
  --disable-asm --disable-x86asm --disable-inline-asm \
  --enable-pic \
  --disable-pthreads --disable-w32threads --disable-os2threads

sed -i 's|#define HAVE_SYSCTL 1|//#define HAVE_SYSCTL 1|' config.h || true

make -j"$(nproc)"
make install
echo "FFmpeg installed at $PREFIX"
