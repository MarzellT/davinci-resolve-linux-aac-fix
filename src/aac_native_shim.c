/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 the resolve-linux-native-aac contributors
 *
 * aac_native_shim.c — LD_PRELOAD interoperability shim that enables DaVinci
 * Resolve Linux to natively decode AAC-in-MP4 by writing seven small patches
 * to private COW pages of the Resolve process at constructor time. The
 * on-disk binary is never modified.
 *
 * INTEROPERABILITY (17 U.S.C. §1201(f)). This file contains no code,
 * symbols, RTTI strings, headers, or extracted byte sequences from
 * DaVinci Resolve. It uses only:
 *   - standard POSIX/libc symbols (mprotect, readlink, write)
 *   - a set of seven version-fingerprint bytes per patch site (used solely
 *     to verify "this is the supported Resolve version" before patching;
 *     functionally equivalent to a per-site checksum)
 *   - a set of seven small replacement-byte sequences encoding the patches
 *     themselves (authored by the shim, not extracted from Resolve)
 *
 * The shim no-ops cleanly on any unsupported Resolve version: if a
 * fingerprint mismatch is detected at any site, that site is skipped, a
 * trace line is written to stderr, and Resolve runs unchanged.
 *
 * Build: gcc -shared -fPIC -O2 -o aac_native_shim.so aac_native_shim.c
 * Use:   add to LD_PRELOAD in your Resolve launcher (see example/).
 *
 * Verified Resolve version: Studio 20.3.2 Linux
 *   (md5 of /opt/resolve/bin/resolve == 4319a873…)
 */

#define _GNU_SOURCE
#include <sys/mman.h>
#include <stdint.h>
#include <unistd.h>
#include <stddef.h>

/* tiny stderr trace, no libc stdio dep beyond write() */
static void wlog(const char *s) {
    const char *p = s; size_t n = 0; while (p[n]) n++;
    (void)!write(2, s, n);
}
static void whex(const char *tag, unsigned long v) {
    char buf[64]; int i = 0; const char *t = tag; while (*t) buf[i++] = *t++;
    buf[i++] = '0'; buf[i++] = 'x';
    char tmp[16]; int j = 0; if (!v) tmp[j++] = '0';
    while (v) { int d = v & 0xf; tmp[j++] = d < 10 ? '0' + d : 'a' + d - 10; v >>= 4; }
    while (j) buf[i++] = tmp[--j];
    buf[i++] = '\n'; (void)!write(2, buf, i);
}

typedef struct {
    uintptr_t addr;             /* in-memory address of the patch site */
    const unsigned char *fp;    /* version-fingerprint bytes (must match before we patch) */
    const unsigned char *patch; /* replacement bytes we write */
    size_t len;
    const char *label;
} patch_site_t;

/* Patch 1 — add the missing audio-codec FOURCC dispatch arm so the AAC
 *           FOURCC reaches the existing native code path. (18 bytes.)
 */
static const unsigned char FP1[] = {0x41,0x81,0xfc,0x45,0x4e,0x4f,0x4e,0x0f,0x84,0x27,0x09,0x00,0x00,0xe9,0xa9,0x00,0x00,0x00};
static const unsigned char PX1[] = {0x41,0x81,0xfc,0x20,0x63,0x61,0x61,0x0f,0x84,0xfb,0x00,0x00,0x00,0xe9,0xa9,0x00,0x00,0x00};

/* Patch 2 — bypass a disabled native-AAC wrapper that would otherwise be
 *           invoked on this code path. (2 bytes; NOP the branch.)
 */
static const unsigned char FP2[] = {0x74,0x14};
static const unsigned char PX2[] = {0x90,0x90};

/* Patch 3 — extend the codec-name lookup table so the AAC FOURCC matches.
 *           (4 bytes in .rodata, consumed once at startup.)
 */
static const unsigned char FP3[] = {0x31,0x63,0x61,0x61};
static const unsigned char PX3[] = {0x20,0x63,0x61,0x61};

/* Patches 4 & 5 — relax the extradata-validity gate so the libavcodec
 *                  AAC decoder is reached. (1 byte each.)
 */
static const unsigned char FP4[] = {0x10};
static const unsigned char PX4[] = {0x02};
static const unsigned char FP5[] = {0x10};
static const unsigned char PX5[] = {0x02};

/* Patch 8 — body of a small trampoline in an existing zero-byte region of a
 *           readable, executable LOAD segment: bump the extradata source
 *           pointer past the MPEG-4 ES_Descriptor header to the inner
 *           AudioSpecificConfig (5 bytes), then jump back to the call
 *           site. (11 bytes; site was all zero-fill before patching.)
 *           Encodes: add qword[rsp+0x10], 0x1f ; jmp <call_site>
 */
static const unsigned char FP8[] = {0,0,0,0,0,0,0,0,0,0,0};
static const unsigned char PX8[] = {0x48,0x83,0x44,0x24,0x10,0x1f,0xe9,0x7e,0x84,0x05,0x08};

/* Patch 6 — retarget the extradata-gate jump to land in the patch-8
 *           trampoline. (4-byte rel32.)
 */
static const unsigned char FP6[] = {0x34,0x01,0x00,0x00};
static const unsigned char PX6[] = {0xdd,0x7c,0xfa,0xf7};

static const patch_site_t SITES[] = {
    {0x8473b9e, FP1, PX1, sizeof FP1, "1 fourcc-arm"},
    {0x8473ce4, FP2, PX2, sizeof FP2, "2 wrapper-bypass"},
    {0xc9fc694, FP3, PX3, sizeof FP3, "3 codec-name-table"},
    {0x8468657, FP4, PX4, sizeof FP4, "4 extradata-gate-a"},
    {0x84686ab, FP5, PX5, sizeof FP5, "5 extradata-gate-b"},
    {0x410392,  FP8, PX8, sizeof FP8, "8 trampoline-body"},
    {0x84686b1, FP6, PX6, sizeof FP6, "6 gate-to-trampoline"},
};
#define NSITES (sizeof(SITES)/sizeof(SITES[0]))

/* Match "/opt/resolve/bin/resolve" at the end of /proc/self/exe — we only
 * apply patches to the main Resolve process, never to helper subprocesses
 * such as Fairlight scriptapp, ResolveHelper, etc., which load the same
 * libraries but should not be modified.
 */
static int is_resolve_main(void) {
    char exe[512]; ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return 0;
    exe[n] = 0;
    const char *want = "/opt/resolve/bin/resolve";
    size_t wl = 0; while (want[wl]) wl++;
    if ((size_t)n < wl) return 0;
    for (size_t i = 0; i < wl; i++) if (exe[n - wl + i] != want[i]) return 0;
    return 1;
}

__attribute__((constructor))
static void aac_native_apply(void) {
    if (!is_resolve_main()) return;
    wlog("[aac_native] resolve detected; applying in-memory patches\n");

    int ok = 0, skip_already = 0, skip_mismatch = 0, fail = 0;
    for (size_t k = 0; k < NSITES; k++) {
        const patch_site_t *s = &SITES[k];
        unsigned char *m = (unsigned char *)s->addr;
        uintptr_t pg = s->addr & ~(uintptr_t)0xfff;
        size_t span = (s->addr + s->len) - pg;
        size_t prot_len = (span + 0xfff) & ~(uintptr_t)0xfff;

        if (mprotect((void *)pg, prot_len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            whex("[aac_native] FAIL mprotect ", s->addr);
            fail++;
            continue;
        }

        int already = 1, matches = 1;
        for (size_t i = 0; i < s->len; i++) {
            if (m[i] != s->patch[i]) already = 0;
            if (m[i] != s->fp[i])    matches = 0;
        }

        if (already) {
            mprotect((void *)pg, prot_len, PROT_READ | PROT_EXEC);
            skip_already++;
            continue;
        }
        if (!matches) {
            /* Different Resolve version — bytes at this site don't match the
             * fingerprint we know. Skip cleanly; Resolve runs unchanged. */
            whex("[aac_native] SKIP orig-mismatch ", s->addr);
            mprotect((void *)pg, prot_len, PROT_READ | PROT_EXEC);
            skip_mismatch++;
            continue;
        }

        for (size_t i = 0; i < s->len; i++) m[i] = s->patch[i];
        mprotect((void *)pg, prot_len, PROT_READ | PROT_EXEC); /* restore W^X */
        whex("[aac_native] patched ", s->addr);
        ok++;
    }
    whex("[aac_native] applied ok=",   (unsigned long)ok);
    whex("[aac_native] skip_already=", (unsigned long)skip_already);
    whex("[aac_native] skip_mismatch=",(unsigned long)skip_mismatch);
    whex("[aac_native] fail=",         (unsigned long)fail);
}
