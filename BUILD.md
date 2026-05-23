# Build, install, smoke-test, uninstall

## Prerequisites

- A lawfully licensed installation of DaVinci Resolve Linux. The shim is
  verified against **Studio 20.3.2** specifically; other versions will
  trigger fingerprint mismatch at every patch site and the shim will no-op
  cleanly (Resolve runs unchanged).
- A C compiler (`gcc` or `clang`) and standard libc headers. No other
  dependencies — the shim uses only `mprotect`, `readlink`, `write`.

## Build

```bash
git clone https://github.com/geekzeino/resolve-linux-native-aac.git
cd resolve-linux-native-aac
gcc -shared -fPIC -O2 -o aac_native_shim.so src/aac_native_shim.c
```

Optional: with warnings.

```bash
gcc -shared -fPIC -O2 -Wall -Wextra -o aac_native_shim.so src/aac_native_shim.c
```

## Install

```bash
install -Dm755 aac_native_shim.so ~/.local/lib/aac_native_shim.so
```

## Smoke test (no Resolve needed)

The shim's constructor refuses to do anything unless `/proc/self/exe` ends
in `/opt/resolve/bin/resolve`. You can safely load it into any other binary
to confirm it loads cleanly and exits its constructor without touching
memory:

```bash
LD_PRELOAD=$HOME/.local/lib/aac_native_shim.so /bin/true
echo "exit code: $? (0 = constructor ran cleanly, no patches applied because /bin/true != resolve)"
```

## Use

Wire it into your Resolve launcher's `LD_PRELOAD`. An example launcher is
in `example/resolve-launcher.sh`. Minimal form:

```bash
#!/bin/bash
export LD_PRELOAD="$HOME/.local/lib/aac_native_shim.so${LD_PRELOAD:+:$LD_PRELOAD}"
exec /opt/resolve/bin/resolve "$@"
```

On launch the shim writes a short trace to `stderr`:

```
[aac_native] resolve detected; applying in-memory patches
[aac_native] patched 0x8473b9e
[aac_native] patched 0x8473ce4
...
[aac_native] applied ok=0x7
[aac_native] skip_already=0x0
[aac_native] skip_mismatch=0x0
[aac_native] fail=0x0
```

`ok=0x7` (seven patches applied) means the shim is active. If you see
`skip_mismatch` for any site, your Resolve version is not the one the
fingerprints were derived against — Resolve will run unchanged (no AAC).
See `docs/REVERSE-ENGINEERING-NOTES.md` for re-deriving on a new version.

## Verify it's working

After launching Resolve via the shim'd launcher, drop an AAC-audio `.mp4`
into the media pool, then a clip on a timeline, and open the Edit or
Fairlight page. With the shim active, you should see:

- The audio waveform rendered immediately, without a several-second freeze.
- No `*.resolve.mp4` sibling file created next to the source.
- `/proc/$(pgrep -x resolve)/maps` showing `aac_native_shim.so` mapped but
  no transcode-related shim alongside it.

## Uninstall

```bash
rm -f ~/.local/lib/aac_native_shim.so
# and remove the LD_PRELOAD line you added to your Resolve launcher
```

Because the shim only modifies process-private COW pages, there is nothing
to "unpatch" on disk. The next time you launch Resolve without the shim,
Resolve runs exactly as shipped by BMD.
