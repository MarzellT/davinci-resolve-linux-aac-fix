#!/usr/bin/env bash
# Installs a SEPARATE opt-in launcher, preserving the normal Resolve launcher.
set -euo pipefail
port_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
stage="$port_dir/native-package"
target="$HOME/.local/share/resolve-native-aac"
(cd -- "$stage" && sha256sum --check SHA256SUMS)
mkdir -p -- "$target" "$HOME/.local/share/applications"
for name in resolve-native-aac aac_native_21.so libavcodec.so.60.3.100 SHA256SUMS LICENSE-shim LICENSE-FFmpeg-LGPLv2.1; do
 cp -- "$stage/$name" "$target/.$name.new.$$"
 mv -- "$target/.$name.new.$$" "$target/$name"
done
chmod +x -- "$target/resolve-native-aac"
# HOME may contain spaces; Desktop Entry Exec uses double-quoted arguments.
python - "$target" "$HOME/.local/share/applications/resolve-native-aac.desktop" <<'PY'
import pathlib, sys

exe = str(pathlib.Path(sys.argv[1]) / "resolve-native-aac")
exe = (
    exe.replace("\\", "\\\\")
    .replace('"', '\\"')
    .replace("`", "\\`")
    .replace("$", "\\$")
)
pathlib.Path(sys.argv[2]).write_text(
    '[Desktop Entry]\nType=Application\nName=DaVinci Resolve (Native AAC)\nComment=Experimental native AAC support for verified Resolve Studio 21.1\nExec="'
    + exe
    + '" %U\nIcon=davinci-resolve\nTerminal=false\nCategories=AudioVideo;Video;\n'
)
PY
printf 'Installed separate Native AAC launcher. Normal Resolve launcher unchanged.\n'
