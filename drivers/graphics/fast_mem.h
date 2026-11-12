#pragma once
/* memset / memcpy for the scanline handlers (DMA IRQ, video timer, video
 * loop).  The C library ones are in the flash: a call there waits for the
 * XIP cache and can make a line late.  These are in the RAM and store words.
 * no-tree-loop-distribute-patterns: GCC would turn the loops back into
 * memset / memcpy calls. */
#include <stdint.h>
#include "pico.h"

#define FAST_MEM_FUNC(name) \
    __attribute__((noinline, unused, optimize("Os", "no-tree-loop-distribute-patterns", "no-ipa-cp-clone"))) __not_in_flash_func(name)

static void FAST_MEM_FUNC(fast_memset)(void* const dst, const uint8_t value, int count) {
    uint8_t* d = (uint8_t*)dst;
    while (count > 0 && ((uintptr_t)d & 3u)) {
        *d++ = value;
        count--;
    }
    const uint32_t v = value * 0x01010101u;
    uint32_t* w = (uint32_t*)d;
    while (count >= 16) {
        w[0] = v;
        w[1] = v;
        w[2] = v;
        w[3] = v;
        w += 4;
        count -= 16;
    }
    while (count >= 4) {
        *w++ = v;
        count -= 4;
    }
    d = (uint8_t*)w;
    while (count > 0) {
        *d++ = value;
        count--;
    }
}

static void FAST_MEM_FUNC(fast_memcpy)(void* const dst, const void* const src, int count) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (!(((uintptr_t)d | (uintptr_t)s) & 3u)) {
        uint32_t* wd = (uint32_t*)d;
        const uint32_t* ws = (const uint32_t*)s;
        while (count >= 16) {
            wd[0] = ws[0];
            wd[1] = ws[1];
            wd[2] = ws[2];
            wd[3] = ws[3];
            wd += 4;
            ws += 4;
            count -= 16;
        }
        while (count >= 4) {
            *wd++ = *ws++;
            count -= 4;
        }
        d = (uint8_t*)wd;
        s = (const uint8_t*)ws;
    }
    while (count > 0) {
        *d++ = *s++;
        count--;
    }
}
