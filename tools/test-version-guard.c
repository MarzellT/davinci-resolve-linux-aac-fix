/* Exercise the actual constructor with an unsupported host hash. No Resolve
 * process is started and no mapped executable page is ever modified. */
#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static ssize_t fake_readlink(const char *path, char *out, size_t n) {
    if (!strcmp(path, "/proc/self/exe")) {
        const char *s = "/opt/resolve/bin/resolve";
        size_t len = strlen(s);
        assert(n >= len);
        memcpy(out, s, len);
        return (ssize_t)len;
    }
    return readlink(path, out, n);
}
static int fake_open(const char *path, int flags, ...) {
    if (!strcmp(path, "/proc/self/exe")) {
        return open("/bin/true", O_RDONLY | O_CLOEXEC);
    }
    return open(path, flags);
}
#define readlink fake_readlink
#define open fake_open
#include "../src/aac_native_21.c"
#undef readlink
#undef open
int main(void) {
    assert(patch_active == 0);
    assert(supplemental_handle == NULL);
    puts("PASS: unsupported host hash rejected before backend loading or memory patching.");
}
