# resolve-linux-native-aac

**Native AAC-in-MP4 decode for DaVinci Resolve on Linux**, delivered as an
LD_PRELOAD interoperability shim. No on-disk binary modification. No FLAC
sibling transcodes. Adds the codec arm that has been missing on Linux since
Resolve 15 (2018).

> **Interoperability statement.** This project is an independently authored
> compatibility layer that interposes standard POSIX/libc/libavcodec symbols
> to enable a lawfully licensed installation of DaVinci Resolve on Linux to
> interoperate with AAC-in-MP4 media files the user lawfully possesses. It
> does **not** circumvent any technological protection measure, contains no
> code or binary fragments extracted from DaVinci Resolve, distributes no
> BlackmagicDesign binaries, and modifies nothing on disk. It is published
> under 17 U.S.C. §1201(f) and the Apache License 2.0.
>
> *DaVinci Resolve and BlackmagicDesign are trademarks of Blackmagic Design
> Pty. Ltd. This project is not affiliated with, endorsed by, or sponsored
> by Blackmagic Design.*

---

## Quick install

```bash
curl -fsSL https://raw.githubusercontent.com/geekzeino/resolve-linux-native-aac/main/install.sh | bash
```

Prefer to read the script first?

```bash
curl -fsSL https://raw.githubusercontent.com/geekzeino/resolve-linux-native-aac/main/install.sh -o install.sh
less install.sh   # review it
bash install.sh
```

The installer is fully scoped to your user account (`~/.local/lib`, `~/.local/bin`, `~/.local/share/applications`) — no root needed, your `/opt/resolve` binary is never modified. After it finishes you'll see a new app-menu entry: **DaVinci Resolve (Native AAC)** alongside the regular one. Click it instead of the default; everything else works the same.

To uninstall:

```bash
curl -fsSL https://raw.githubusercontent.com/geekzeino/resolve-linux-native-aac/main/uninstall.sh | bash
```

---

## TL;DR

| | Before this shim | With this shim |
|---|---|---|
| AAC import latency | ~9 s/hr of video (community ffmpeg-FLAC transcode workaround) | **0 — Resolve reads the AAC directly** |
| `*.resolve.mp4` FLAC sibling files | yes (one per source) | **none** |
| On-disk Resolve binary | unchanged | **unchanged** (in-memory COW only) |
| Resolve update safety | works (workaround is generic) | shim no-ops cleanly on version mismatch — falls back to original behavior |

---

## Background

Resolve on Linux does not include an AAC decoder, by BlackmagicDesign's own
explanation: licensing. From the [official BMD forum thread][bmd-aac]:

> *"the simple answer is due to licensing"* — Peter Chamberlain, BMD

Every Linux Resolve user importing camera footage (Canon, GoPro, smartphones,
screen-capture) hits silent audio on first import. Eight years of forum
threads, an [ArchWiki section][archwiki], [editorial coverage][provideo], and
~98 ⭐ on a [community AAC *encoder* plugin][toxblh] all confirm the
pain. The standard community workaround pre-transcodes the audio to a FLAC
sibling and tells Resolve to read that instead — functional, but slow on
first import and clutters project directories.

This shim takes a different path: it patches the Resolve binary **in memory**
at process startup so the codec dispatch reaches Resolve's own dormant native
AAC builder, supplied with the right MPEG-4 AudioSpecificConfig. The result
is identical to how AAC works on the macOS/Windows builds — native decode
on import, instant waveform, no sibling files.

[bmd-aac]: https://forum.blackmagicdesign.com/viewtopic.php?f=33&t=102620
[archwiki]: https://wiki.archlinux.org/title/DaVinci_Resolve
[provideo]: https://www.provideocoalition.com/aac-audio-kdenlive-beats-davinci-resolve-studio-on-linux/
[toxblh]: https://github.com/Toxblh/davinci-linux-aac-codec

---

## How it works

DaVinci Resolve on Linux ships with a working `libavcodec.so.60` and a
audio-codec dispatcher that routes FOURCC tokens (`mp3 `, `flac`,
`opus`, …) to libavcodec. It also ships a dormant native-AAC code path
(builder + decode entry) — but the FOURCC switch has no `aac ` arm to reach
it, and the binary's `esds`-to-extradata copy hands libavcodec the wrong
descriptor bytes (the 42-byte ES_Descriptor instead of the 5-byte
AudioSpecificConfig at offset 0x1f), causing the AAC decoder to return
`-38 / "Audio object type 0 not implemented"`.

The shim does seven small in-memory byte changes that together: (a) add the
missing `aac ` arm to the FOURCC switch, (b) route it past the disabled
wrapper, (c) supply the correct AudioSpecificConfig pointer (+0x1f past the
esds header) via a short trampoline in an existing zero-byte region of a
readable+executable LOAD segment.

Net effect: Resolve's own existing libavcodec call site decodes AAC packets
the same way it decodes FLAC, MP3 and Opus. Peak generation + playback both
go through the native path. No second decoder is shipped or linked.

The shim:

- runs as a `__attribute__((constructor))` at `ld.so` load time,
- verifies it's loaded into `/opt/resolve/bin/resolve` and no other process,
- verifies seven small byte-signatures match the supported Resolve version
  before writing anything (mismatch → skips, logs, no-op),
- uses `mprotect()` to make the necessary pages writable, applies the patch
  bytes, then restores `PROT_READ|PROT_EXEC`,
- touches **zero** files on disk; the only effect is in this process's
  private COW pages, which vanish when Resolve exits.

---

## Supported Resolve version

| Resolve | Binary md5 | Status |
|---|---|---|
| Studio 20.3.2 Linux | `4319a87312…` (md5 of `/opt/resolve/bin/resolve`) | **Verified** |
| Other versions | — | not yet supported — see "Update safety" |

Verified across LC AAC mono 44.1k, LC AAC stereo 48k, dynamic-envelope
LC AAC. PFL peak-file correlation with `ffmpeg` reference: r ≈ 0.999994.

### Known limitations (use the original community transcode workaround for these)

| AAC variant | Behavior with this shim |
|---|---|
| LC AAC (mono, stereo, 44.1k, 48k, …) — **what real-world muxers emit** | works natively |
| HE-AAC SBR (true `objectType=5` ASC) | crashes Resolve (use ffmpeg pre-transcode) |
| Compact-length-esds AAC (1-byte MPEG-4 descriptor lengths instead of 4-byte) | crashes Resolve on import — a proven use-after-free in Resolve's peak-gen path beyond the shim's reach |

ffmpeg, x264, Apple, and most consumer encoders emit the 4-byte-length esds
that works. The compact-length case is rare in the wild but legal MPEG-4;
keep the ffmpeg-FLAC transcode workaround available for those files.

### Update safety

The shim verifies the bytes at each patch site against a known-version
signature before writing. On a Resolve update (different binary, different
offsets), all signatures mismatch, the constructor logs `SKIP orig-mismatch`
for each site, and exits without writing. Resolve then runs as if the shim
weren't loaded — AAC reverts to the baseline silent-import behavior. **The
shim never crashes Resolve on an unsupported version**; it just no-ops.

To restore native AAC after a Resolve update, the patch sites need to be
re-derived for the new binary (the LOAD-segment layout and the audio-codec dispatcher and
surrounding code shift). See [docs/REVERSE-ENGINEERING-NOTES.md](docs/REVERSE-ENGINEERING-NOTES.md).

---

## Build

```bash
gcc -shared -fPIC -O2 -o aac_native_shim.so src/aac_native_shim.c
install -Dm755 aac_native_shim.so ~/.local/lib/aac_native_shim.so
```

That's it — no dependencies beyond `libc` (`mprotect`, `readlink`, `write`).
No bundled AAC decoder. No bundled libavcodec. The shim calls
**Resolve's own** bundled `libavcodec.so.60` via Resolve's own existing call
sites — nothing about AAC patent licensing changes vs. what BMD already ships.

## Use

Add the shim to your Resolve launcher's `LD_PRELOAD`. A complete example
launcher is in [`example/resolve-launcher.sh`](example/resolve-launcher.sh).
Minimal form:

```bash
#!/bin/bash
export LD_PRELOAD="$HOME/.local/lib/aac_native_shim.so${LD_PRELOAD:+:$LD_PRELOAD}"
exec /opt/resolve/bin/resolve "$@"
```

On launch the shim writes a short trace to `stderr`:

```
[aac_native] resolve detected; applying in-memory cave patches
[aac_native] patched 0x8473b9e
[aac_native] patched 0x8473ce4
...
[aac_native] applied ok=0x7  [aac_native] skip=0x0  [aac_native] fail=0x0
```

`ok=0x7` (seven patches applied) means the shim is active. Drop an AAC clip
on a timeline and confirm the waveform appears with no `.resolve.mp4`
sibling created next to the source.

---

## Project files

- [`src/aac_native_shim.c`](src/aac_native_shim.c) — the shim source (~100 lines, no external deps).
- [`example/resolve-launcher.sh`](example/resolve-launcher.sh) — example Resolve launcher with the shim wired in.
- [`docs/REVERSE-ENGINEERING-NOTES.md`](docs/REVERSE-ENGINEERING-NOTES.md) — how the patch sites were derived; useful when porting to a new Resolve version.
- [`BUILD.md`](BUILD.md) — build, install, smoke-test, and uninstall steps.

---

## What this project is — and isn't

- **Is**: an independently authored compatibility shim that lets Resolve's
  *own* bundled libavcodec decode AAC, by adding the missing FOURCC arm and
  fixing the `esds → AudioSpecificConfig` pointer.
- **Isn't**: a Resolve crack, a license bypass, a DRM circumvention tool, or
  a redistribution of any BlackmagicDesign binary. It contains no code,
  binaries, byte fragments, headers, symbols, or RTTI strings extracted
  from `/opt/resolve/bin/resolve` — only a small set of **patch-site
  fingerprint bytes** (used to verify "this is the supported Resolve
  version" before patching) and the small set of **replacement bytes** that
  encode the actual patch instructions. You must already own a lawful copy
  of DaVinci Resolve to use this shim; nothing here helps you obtain one.

The shim is what wine, dxvk, vkd3d, youtube-dl, and similar interoperability
projects are: standard POSIX function-interposition (`LD_PRELOAD`) used to
enable two independently authored programs (Resolve and libavcodec) to talk
through a missing arm in one of them.

## Patent / licensing note (read this)

AAC is patent-encumbered. Most Linux distributions ship a `libavcodec`
package whose AAC support is licensed (or whose licensing position is
documented by the distribution). **This shim does not ship an AAC decoder.**
It calls into the `libavcodec.so.60` that **BlackmagicDesign already ships
with Resolve** via Resolve's own existing call sites. Whatever the AAC
patent-licensing position is for the libavcodec that BMD ships, it doesn't
change here. If your jurisdiction or workflow imposes additional AAC patent
obligations on you, that's separately your responsibility.

## License

Apache License 2.0 — see [LICENSE](LICENSE). The Apache 2.0 patent grant
applies to contributions by the authors of this shim; it does not (and
cannot) grant AAC patents held by third parties.

## Contributing

Issues and PRs welcome. The two most-wanted contributions:

1. A hybrid mode that auto-detects compact-length-esds / HE-AAC SBR ASC at
   `open()` time and falls back to the existing FLAC-sibling transcode for
   those (zero regression vs the workaround on edge cases).
2. Re-derivation of the patch sites for newer Resolve versions when BMD
   releases one.

Please do not file issues with "crack", "bypass", "unlock", "activation",
or "license" framing. Issues that aren't strictly about codec interoperability
will be closed.
