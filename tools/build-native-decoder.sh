#!/usr/bin/env bash
# Build a supplemental AAC-only codec. Never writes /opt/resolve or changes its libs.
set -euo pipefail
build_root=${1:-"$HOME/.local/state/resolve-aac-port-build"}
mkdir -p -- "$build_root"
build_root=$(CDPATH= cd -- "$build_root" && pwd)
archive="$build_root/ffmpeg-6.0.tar.xz"
if [[ ! -f "$archive" ]]; then
 curl --fail --location --output "$archive.tmp" https://ffmpeg.org/releases/ffmpeg-6.0.tar.xz
 mv -- "$archive.tmp" "$archive"
fi
printf '%s  %s\n' 57be87c22d9b49c112b6d24bc67d42508660e6b718b3db89c44e47e289137082 "$archive" | sha256sum --check -
if [[ ! -d "$build_root/ffmpeg-6.0" ]]; then tar -xf "$archive" -C "$build_root"; fi
cd -- "$build_root/ffmpeg-6.0"
./configure --prefix="$build_root/decoder" --disable-everything --enable-decoder=aac \
 --enable-shared --disable-static --disable-programs --disable-doc --disable-avdevice \
 --disable-avformat --disable-avfilter --disable-swscale --disable-swresample \
 --disable-postproc --disable-network --disable-x86asm --disable-inline-asm \
 > "$build_root/configure.log"
make -j4 > "$build_root/build.log" 2>&1
make install >> "$build_root/build.log" 2>&1
# Resolve already supplies matching libavutil58. $ORIGIN also allows standalone
# verification against the freshly built matching pair without global changes.
patchelf --set-rpath '$ORIGIN' "$build_root/decoder/lib/libavcodec.so.60.3.100"
printf 'Built isolated AAC decoder: %s\n' "$build_root/decoder/lib/libavcodec.so.60.3.100"
