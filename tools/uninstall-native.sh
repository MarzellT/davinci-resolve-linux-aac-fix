#!/usr/bin/env bash
set -euo pipefail
target="$HOME/.local/share/resolve-native-aac"
rm -f -- "$HOME/.local/share/applications/resolve-native-aac.desktop"
for name in resolve-native-aac aac_native_21.so libavcodec.so.60.3.100 SHA256SUMS LICENSE-shim LICENSE-FFmpeg-LGPLv2.1; do
 rm -f -- "$target/$name"
done
rmdir -- "$target" 2>/dev/null || true
printf 'Native AAC launcher removed. Restart Resolve using its normal launcher. Media and projects unchanged.\n'
