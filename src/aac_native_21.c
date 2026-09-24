#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* These ABI offsets and call sites belong to the hash-verified host only. */
enum {
    AAC_CODEC_ID = 0x15002,
    CODEC_ID_OFFSET = 24,
    EXTRADATA_OFFSET = 88,
    EXTRADATA_SIZE_OFFSET = 96,
    FFMPEG_6_CODEC_VERSION = (60u << 16) | (3u << 8) | 100u,
    RESOLVE_AAC_OPEN_RETURN = 0x5dcb6d1,
    RESOLVE_AAC_LOOKUP_RETURN = 0x5dcb46b,
};

/* This is an isolated native-only port, not the upstream file-conversion shim.
 * The only supported host is Studio 21.1.0.17 Linux x86_64 with this full hash.
 * File bytes, container parsing, packet timestamps and DJI data stay untouched. */
static const unsigned char supported_host_sha256[32] = {
    0x99, 0xd4, 0xe0, 0xa4, 0x6d, 0x63, 0xcb, 0xfe, 0x71, 0xce, 0x4b, 0x30, 0xf2, 0x3c, 0x56, 0x54,
    0xff, 0xbd, 0xd1, 0x7d, 0x78, 0x2d, 0x93, 0x68, 0x98, 0x6c, 0xec, 0xd1, 0x59, 0xe4, 0x15, 0xd0};
extern const unsigned char aac_stubs_start[], aac_stubs_end[], aac_dispatch[], aac_lookup[],
    aac_gate1[], aac_gate2[];
static int patch_active;
struct codec_api {
    const void *(*find)(int);
    void *(*alloc)(const void *);
    int (*open)(void *, const void *, void *);
    int (*send)(void *, const void *);
    int (*receive)(void *, void *);
    void (*flush)(void *);
    int (*close)(void *);
    void (*free_ctx)(void **);
    unsigned int (*version)(void);
};
static struct codec_api original, supplemental;
static const void *supplemental_aac;
static void *supplemental_handle;
static pthread_once_t api_once = PTHREAD_ONCE_INIT;
struct owned_context {
    void *ctx;
    struct owned_context *next;
};
static struct owned_context *owned_contexts;
static pthread_mutex_t owned_contexts_lock = PTHREAD_MUTEX_INITIALIZER;
static void load_api(void *handle, struct codec_api *api) {
    api->find = dlsym(handle, "avcodec_find_decoder");
    api->alloc = dlsym(handle, "avcodec_alloc_context3");
    api->open = dlsym(handle, "avcodec_open2");
    api->send = dlsym(handle, "avcodec_send_packet");
    api->receive = dlsym(handle, "avcodec_receive_frame");
    api->flush = dlsym(handle, "avcodec_flush_buffers");
    api->close = dlsym(handle, "avcodec_close");
    api->free_ctx = dlsym(handle, "avcodec_free_context");
    api->version = dlsym(handle, "avcodec_version");
}

static void resolve_api(void) {
    load_api(RTLD_NEXT, &original);
}

static int api_complete(struct codec_api *api) {
    return api->find && api->alloc && api->open && api->send && api->receive && api->flush &&
           api->close && api->free_ctx && api->version;
}

static int is_supplemental_context(void *ctx) {
    int result = 0;
    pthread_mutex_lock(&owned_contexts_lock);
    for (struct owned_context *n = owned_contexts; n; n = n->next) {
        if (n->ctx == ctx) {
            result = 1;
            break;
        }
    }
    pthread_mutex_unlock(&owned_contexts_lock);
    return result;
}

static void log_message(const char *s) {
    write(2, s, strlen(s));
}
struct patch {
    uintptr_t address;
    size_t length;
    const unsigned char *fingerprint, *stub;
    unsigned char replacement[16];
};
static const unsigned char dispatch_fingerprint[] = {0xe9, 0xac, 0, 0, 0};
static const unsigned char lookup_fingerprint[] = {0x4c, 0x8b, 0x05, 0xd8, 0x3d, 0x9e, 0x23};
static const unsigned char extradata_gate_fingerprint[] = {0x81, 0xf9, 0x10, 0x50,
                                                           0x01, 0,    0x74, 0x0c};
static const unsigned char copy_gate_fingerprint[] = {0x81, 0xff, 0x10, 0x50, 0x01, 0,
                                                      0x0f, 0x84, 0xed, 0,    0,    0};
static struct patch patches[] = {
    {0x5dd63da, sizeof dispatch_fingerprint, dispatch_fingerprint, aac_dispatch, {0}},
    {0x5dcb2b9, sizeof lookup_fingerprint, lookup_fingerprint, aac_lookup, {0}},
    {0x5dcb48f, sizeof extradata_gate_fingerprint, extradata_gate_fingerprint, aac_gate1, {0}},
    {0x5dcb4e5, sizeof copy_gate_fingerprint, copy_gate_fingerprint, aac_gate2, {0}},
};
static int host_hash_matches(void) {
    int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    SHA256_CTX hash_context;
    unsigned char digest[32], buffer[65536];
    ssize_t bytes_read;
    SHA256_Init(&hash_context);
    while ((bytes_read = read(fd, buffer, sizeof buffer)) > 0) {
        SHA256_Update(&hash_context, buffer, (size_t)bytes_read);
    }
    close(fd);
    if (bytes_read < 0) {
        return 0;
    }
    SHA256_Final(digest, &hash_context);
    return !memcmp(digest, supported_host_sha256, 32);
}

static int page_protection(uintptr_t page, size_t pagesize) {
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) {
        return -1;
    }
    char line[PATH_MAX + 256], permissions[5], path[PATH_MAX];
    unsigned long start, end, offset, inode;
    unsigned int device_major, device_minor;
    int found = -1;
    while (fgets(line, sizeof line, maps)) {
        path[0] = 0;
        if (sscanf(line, "%lx-%lx %4s %lx %x:%x %lu %4095s", &start, &end, permissions, &offset,
                   &device_major, &device_minor, &inode, path) == 8 &&
            page >= start && page + pagesize <= end && !strcmp(path, "/opt/resolve/bin/resolve")) {
            found = (permissions[0] == 'r' ? PROT_READ : 0) |
                    (permissions[1] == 'w' ? PROT_WRITE : 0) |
                    (permissions[2] == 'x' ? PROT_EXEC : 0);
            break;
        }
    }
    fclose(maps);
    return found;
}

__attribute__((constructor)) static void init_native(void) {
    char exe[PATH_MAX];
    ssize_t path_length = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (path_length < 0) {
        return;
    }
    exe[path_length] = 0;
    if (strcmp(exe, "/opt/resolve/bin/resolve")) {
        return;
    }
    if (!host_hash_matches()) {
        log_message("[aac_native21] Unsupported Resolve SHA256; no patches applied.\n");
        return;
    }
    pthread_once(&api_once, resolve_api);
    if (!api_complete(&original) || (original.version() >> 16) != 60) {
        log_message("[aac_native21] Unsupported decoder ABI; no patches applied.\n");
        return;
    }
    if (!original.find(AAC_CODEC_ID)) {
        const char *path = getenv("RESOLVE_AAC_DECODER_LIBRARY");
        if (!path || path[0] != '/') {
            log_message("[aac_native21] Bundled AAC decoder absent; supplemental library required. "
                        "No patches applied.\n");
            return;
        }
        /* A separate codec implementation with the exact FFmpeg 6.0 ABI, kept local.
         * Existing libavutil58 stays shared for AVFrame and buffer allocation.
         * Deep binding keeps the supplemental codec's internal calls self-contained.
         * Only registered AAC contexts are ever routed to it; all video/other audio
         * functions continue to use the original bundled codec implementation. */
        supplemental_handle = dlopen(path, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
        if (supplemental_handle) {
            load_api(supplemental_handle, &supplemental);
        }
        if (!api_complete(&supplemental) || supplemental.version() != FFMPEG_6_CODEC_VERSION ||
            !(supplemental_aac = supplemental.find(AAC_CODEC_ID))) {
            log_message("[aac_native21] Supplemental AAC decoder failed ABI/load checks; no "
                        "patches applied.\n");
            if (!supplemental_handle) {
                const char *err = dlerror();
                if (err) {
                    log_message(err);
                }
            }
            return;
        }
        log_message("[aac_native21] Supplemental FFmpeg 6.0 AAC decoder loaded locally; existing "
                    "codecs retained.\n");
    }
    /* Verify every target before allocating trampolines or modifying code. */
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE), count = sizeof patches / sizeof patches[0];
    uintptr_t pages[4];
    int protections[4];
    size_t page_count = 0;
    for (size_t i = 0; i < count; i++) {
        struct patch *patch = &patches[i];
        uintptr_t page = patch->address & ~(page_size - 1);
        int protection = page_protection(page, page_size);
        if (protection != (PROT_READ | PROT_EXEC) ||
            (patch->address + patch->length - 1) / page_size != patch->address / page_size ||
            memcmp((void *)patch->address, patch->fingerprint, patch->length)) {
            log_message("[aac_native21] Fingerprint/protection preflight failed; no patches "
                        "applied.\n");
            return;
        }
        size_t j;
        for (j = 0; j < page_count && pages[j] != page; j++)
            ;
        if (j == page_count) {
            pages[page_count] = page;
            protections[page_count++] = protection;
        }
    }
    /* Copy position-independent stubs into a private mapping, then seal it RX. */
    size_t stub_size = (size_t)(aac_stubs_end - aac_stubs_start),
           allocation_size = (stub_size + page_size - 1) & ~(page_size - 1);
    unsigned char *stub = mmap(NULL, allocation_size, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (stub == MAP_FAILED) {
        log_message("[aac_native21] Cannot allocate trampolines; no patches applied.\n");
        return;
    }
    memcpy(stub, aac_stubs_start, stub_size);
    if (mprotect(stub, allocation_size, PROT_READ | PROT_EXEC)) {
        munmap(stub, allocation_size);
        return;
    }
    for (size_t i = 0; i < count; i++) {
        struct patch *patch = &patches[i];
        intptr_t destination = (intptr_t)(stub + (patch->stub - aac_stubs_start));
        int64_t delta = destination - (int64_t)(patch->address + 5);
        if (delta < INT32_MIN || delta > INT32_MAX) {
            munmap(stub, allocation_size);
            return;
        }
        memset(patch->replacement, 0x90, patch->length);
        patch->replacement[0] = 0xe9;
        int32_t relative_offset = (int32_t)delta;
        memcpy(patch->replacement + 1, &relative_offset, 4);
    }
    /* Acquire all writable pages before committing any detour. */
    size_t changed = 0;
    for (; changed < page_count; changed++) {
        if (mprotect((void *)pages[changed], page_size, PROT_READ | PROT_WRITE)) {
            break;
        }
    }
    if (changed != page_count) {
        for (size_t j = 0; j < changed; j++) {
            if (mprotect((void *)pages[j], page_size, protections[j])) {
                _exit(125);
            }
        }
        munmap(stub, allocation_size);
        log_message("[aac_native21] Cannot make patch pages writable; no patches applied.\n");
        return;
    }
    for (size_t i = 0; i < count; i++) {
        memcpy((void *)patches[i].address, patches[i].replacement, patches[i].length);
    }
    for (size_t j = 0; j < page_count; j++) {
        if (mprotect((void *)pages[j], page_size, protections[j])) {
            /* Never continue execution with a partially applied patch or altered W^X. */
            log_message("[aac_native21] Fatal page protection restoration error.\n");
            _exit(125);
        }
    }
    patch_active = 1;
    log_message("[aac_native21] Native AAC port active: Studio 21.1.0.17, four verified "
                "detours, original RX protections restored. No file redirection.\n");
}

/* ISO/IEC 14496-1 expandable lengths. Bound every field against its parent's
 * descriptor, support 1..4-byte lengths and the optional ES header fields. */
static int read_descriptor(const unsigned char *data, size_t end, size_t *pos,
                           unsigned int expected_tag, size_t *descriptor_end) {
    if (*pos >= end || data[(*pos)++] != expected_tag) {
        return 0;
    }
    size_t length = 0;
    unsigned int length_byte;
    for (length_byte = 0; length_byte < 4; length_byte++) {
        if (*pos >= end) {
            return 0;
        }
        unsigned char byte = data[(*pos)++];
        length = (length << 7) | (byte & 127);
        if (!(byte & 128)) {
            break;
        }
    }
    if (length_byte == 4 || length > end - *pos) {
        return 0;
    }
    *descriptor_end = *pos + length;
    return 1;
}

int aac_extract_asc(const unsigned char *data, size_t size, size_t *asc_offset,
                    size_t *asc_length) {
    /* Resolve supplies the esds full-box_size payload (version/flags then ES tag),
     * but tolerate an outer esds atom and tag-only payload for a pure parser test. */
    size_t position = 0, end = size, es_end, decoder_config_end, asc_end;
    if (size >= 8 && !memcmp(data + 4, "esds", 4)) {
        size_t box_size =
            ((size_t)data[0] << 24) | ((size_t)data[1] << 16) | ((size_t)data[2] << 8) | data[3];
        if (box_size < 12 || box_size > size) {
            return 0;
        }
        end = box_size;
        position = 8;
    }
    if (end - position >= 5 && data[position] == 0 && data[position + 1] == 0 &&
        data[position + 2] == 0 && data[position + 3] == 0) {
        position += 4;
    }
    if (!read_descriptor(data, end, &position, 3, &es_end) || es_end - position < 3) {
        return 0;
    }
    position += 2;
    unsigned char flags = data[position++];
    if (flags & 128) {
        if (es_end - position < 2) {
            return 0;
        }
        position += 2;
    }
    if (flags & 64) {
        if (position >= es_end) {
            return 0;
        }
        size_t url_length = data[position++];
        if (url_length > es_end - position) {
            return 0;
        }
        position += url_length;
    }
    if (flags & 32) {
        if (es_end - position < 2) {
            return 0;
        }
        position += 2;
    }
    if (!read_descriptor(data, es_end, &position, 4, &decoder_config_end) ||
        decoder_config_end - position < 13) {
        return 0;
    }
    if (data[position] != 0x40 || ((data[position + 1] >> 2) & 63) != 5) {
        return 0; /* MPEG-4 audio */
    }
    position += 13;
    if (!read_descriptor(data, decoder_config_end, &position, 5, &asc_end) ||
        asc_end - position < 2) {
        return 0;
    }
    *asc_offset = position;
    *asc_length = asc_end - position;
    return 1;
}
/* libavcodec60 AVCodecContext ABI used by this exact Resolve build. These
 * offsets are independently evident in its allocator/copy/open instructions. */
int avcodec_open2(void *ctx, const void *codec, void *opts) {
    pthread_once(&api_once, resolve_api);
    if (!original.open) {
        return -ENOSYS;
    }
    if (patch_active && __builtin_return_address(0) == (void *)RESOLVE_AAC_OPEN_RETURN && ctx &&
        *(int *)((char *)ctx + CODEC_ID_OFFSET) == AAC_CODEC_ID) {
        unsigned char *data = *(unsigned char **)((char *)ctx + EXTRADATA_OFFSET);
        int *size = (int *)((char *)ctx + EXTRADATA_SIZE_OFFSET);
        if (data && *size > 0) {
            size_t off = 0, len = 0;
            if (aac_extract_asc(data, (size_t)*size, &off, &len)) {
                memmove(data, data + off, len);
                memset(data + len, 0, (size_t)*size - len);
                *size = (int)len;
                log_message("[aac_native21] Extracted bounded AAC AudioSpecificConfig; opening "
                            "native decoder.\n");
            } else {
                log_message("[aac_native21] AAC descriptor rejected; refusing unsafe decoder "
                            "input.\n");
                return -EINVAL;
            }
        } else {
            log_message("[aac_native21] AAC extradata missing; refusing decoder.\n");
            return -EINVAL;
        }
    }
    return is_supplemental_context(ctx) ? supplemental.open(ctx, codec, opts)
                                        : original.open(ctx, codec, opts);
}

/* Public libavcodec entry points used by Resolve's audio wrapper. */
const void *avcodec_find_decoder(int id) {
    pthread_once(&api_once, resolve_api);
    if (patch_active && supplemental_aac && id == AAC_CODEC_ID &&
        __builtin_return_address(0) == (void *)RESOLVE_AAC_LOOKUP_RETURN) {
        return supplemental_aac;
    }
    return original.find ? original.find(id) : NULL;
}

void *avcodec_alloc_context3(const void *codec) {
    pthread_once(&api_once, resolve_api);
    if (patch_active && supplemental_aac && codec == supplemental_aac) {
        struct owned_context *n = malloc(sizeof *n);
        if (!n) {
            return NULL;
        }
        void *ctx = supplemental.alloc(codec);
        if (!ctx) {
            free(n);
            return NULL;
        }
        n->ctx = ctx;
        pthread_mutex_lock(&owned_contexts_lock);
        n->next = owned_contexts;
        owned_contexts = n;
        pthread_mutex_unlock(&owned_contexts_lock);
        return ctx;
    }
    return original.alloc ? original.alloc(codec) : NULL;
}

int avcodec_send_packet(void *ctx, const void *packet) {
    pthread_once(&api_once, resolve_api);
    return is_supplemental_context(ctx) ? supplemental.send(ctx, packet)
                                        : (original.send ? original.send(ctx, packet) : -ENOSYS);
}

int avcodec_receive_frame(void *ctx, void *frame) {
    pthread_once(&api_once, resolve_api);
    return is_supplemental_context(ctx)
               ? supplemental.receive(ctx, frame)
               : (original.receive ? original.receive(ctx, frame) : -ENOSYS);
}

void avcodec_flush_buffers(void *ctx) {
    pthread_once(&api_once, resolve_api);
    if (is_supplemental_context(ctx)) {
        supplemental.flush(ctx);
    } else if (original.flush) {
        original.flush(ctx);
    }
}

int avcodec_close(void *ctx) {
    pthread_once(&api_once, resolve_api);
    return is_supplemental_context(ctx) ? supplemental.close(ctx)
                                        : (original.close ? original.close(ctx) : -ENOSYS);
}

void avcodec_free_context(void **ctx) {
    pthread_once(&api_once, resolve_api);
    struct owned_context *removed = NULL;
    if (ctx && *ctx) {
        pthread_mutex_lock(&owned_contexts_lock);
        struct owned_context **p = &owned_contexts;
        while (*p) {
            if ((*p)->ctx == *ctx) {
                removed = *p;
                *p = removed->next;
                break;
            }
            p = &(*p)->next;
        }
        pthread_mutex_unlock(&owned_contexts_lock);
    }
    if (removed) {
        supplemental.free_ctx(ctx);
        free(removed);
    } else if (original.free_ctx) {
        original.free_ctx(ctx);
    }
}
