#!/bin/bash
# Example DaVinci Resolve launcher with the native-AAC interoperability shim.
#
# Adapt $HOME/.local/lib/aac_native_shim.so to wherever you installed the shim.
# Adapt any other LD_PRELOAD entries (e.g. distro-specific compatibility shims)
# alongside.

# Some users have reported needing this to avoid an ICE/libSM startup crash:
unset SESSION_MANAGER

# Optional: pin Resolve to a specific NVIDIA GPU UUID. Either keep this line
# with your own UUID (find it via `nvidia-smi -L`), set to "" to let Resolve
# pick, or delete the line entirely.
# export CUDA_VISIBLE_DEVICES=GPU-XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX

export LD_PRELOAD="$HOME/.local/lib/aac_native_shim.so${LD_PRELOAD:+:$LD_PRELOAD}"

# Some users find that disabling IBus avoids hotkey/IME flakiness in Resolve.
# Keep, edit, or remove as your distro/IME setup requires.
export GTK_IM_MODULE=""
export QT_IM_MODULE=""
export XMODIFIERS=""

exec /opt/resolve/bin/resolve "$@"
