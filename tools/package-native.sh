#!/usr/bin/env bash
# Stage a package only: does not modify the desktop or launch Resolve.
set -euo pipefail
port_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
decoder_prefix=${1:-"$HOME/.local/state/resolve-aac-port-build/decoder"}
stage="$port_dir/native-package"
mkdir -p -- "$stage"
cp -- "$port_dir/example/resolve-native-aac" "$stage/resolve-native-aac"
chmod +x -- "$stage/resolve-native-aac"
gcc -shared -fPIC -O2 -Wall -Wextra -Werror -Wno-deprecated-declarations \
  -o "$stage/aac_native_21.so.tmp" "$port_dir/src/aac_native_21.c" "$port_dir/src/aac_native_21.S" -ldl -lcrypto -pthread
mv -- "$stage/aac_native_21.so.tmp" "$stage/aac_native_21.so"
cp -- "$decoder_prefix/lib/libavcodec.so.60.3.100" "$stage/libavcodec.so.60.3.100"
cp -- "$port_dir/LICENSE" "$stage/LICENSE-shim"
cp -- "$decoder_prefix/../ffmpeg-6.0/COPYING.LGPLv2.1" "$stage/LICENSE-FFmpeg-LGPLv2.1"
(cd -- "$stage" && sha256sum aac_native_21.so libavcodec.so.60.3.100 > SHA256SUMS)
printf 'Staged native package: %s\n' "$stage"
