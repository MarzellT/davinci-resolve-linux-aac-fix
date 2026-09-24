#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int aac_extract_asc(const unsigned char *, size_t, size_t *, size_t *);
static const unsigned char nano[] = {
    0, 0, 0,    0x33, 'e',  's',  'd',  's',  0,    0,    0,    0, 3,    0x80, 0x80, 0x80, 0x22,
    0, 2, 0,    4,    0x80, 0x80, 0x80, 0x14, 0x40, 0x15, 0,    0, 0,    0,    4,    0xd7, 0xbf,
    0, 4, 0xd7, 0xbf, 5,    0x80, 0x80, 0x80, 2,    0x11, 0x90, 6, 0x80, 0x80, 0x80, 1,    2};
int main(void) {
    size_t off, len;
    assert(aac_extract_asc(nano, sizeof nano, &off, &len) && len == 2 && nano[off] == 0x11 &&
           nano[off + 1] == 0x90);
    for (size_t n = 0; n < sizeof nano; n++) {
        assert(!aac_extract_asc(nano, n, &off, &len));
    }
    const unsigned char compact[] = {3,    25,   0, 2, 0,    4,    17, 0x40, 0x15, 0,    0, 0, 0, 4,
                                     0xd7, 0xbf, 0, 4, 0xd7, 0xbf, 5,  2,    0x11, 0x90, 6, 1, 2};
    assert(aac_extract_asc(compact, sizeof compact, &off, &len) && len == 2 &&
           compact[off] == 0x11);
    unsigned char buf[128];
    srand(1);
    for (int i = 0; i < 200000; i++) {
        size_t n = (size_t)rand() % sizeof buf;
        for (size_t j = 0; j < n; j++) {
            buf[j] = (unsigned char)rand();
        }
        if (i % 2 == 0) {
            memcpy(buf, nano, sizeof nano);
            n = sizeof nano;
            buf[rand() % n] = (unsigned char)rand();
        }
        if (aac_extract_asc(buf, n, &off, &len)) {
            assert(off <= n && len <= n - off && len >= 2);
        }
    }
    puts("Native ESDS parser: Nano, compact, truncated and 200000 malformed/mutated input checks "
         "passed.");
}
