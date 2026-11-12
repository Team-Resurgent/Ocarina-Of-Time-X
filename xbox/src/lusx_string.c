/* lusx_string.c -- memcmp, memmove and memset for the whole image: libd3d8 and libdsound take them too.
 *
 * The RXDK's are one rep string instruction at every size (rep cmpsb, rep movsb, rep stosb), which runs a byte at a
 * time unless the addresses happen to suit the P6's fast strings, and pays a microcode start on every call. They're
 * on the hot paths: the batch and state compares of the display list (memcmp over ~200 bytes a triangle or a vertex
 * load), every std::vector insert (memmove), the constants' known flags (memset). Melee-X and Prime-X replaced the
 * same routines (pdclib's byte loops: ~40% of a match's frame; 8% of a room's on small rep movs).
 *
 * Their memcpy stays: it's SSE1, with small sizes done in registers, and it looks at %fs:0x24 first to keep out of
 * the XMM registers when it can't use them. That's why these use none: the attribute below keeps the compiler from
 * vectorizing them, as they can be reached from the same places. no_builtin keeps it from turning a loop back into a
 * call to the function it's in.
 */
#include <stddef.h>
#include <stdint.h>

#define LUSX_INTEGER_ONLY __attribute__((target("no-sse,no-mmx"), no_builtin))

typedef uint32_t __attribute__((may_alias, aligned(1))) Word;

/* below this a loop of stores beats a rep's start */
#define LUSX_REP_FROM 64

LUSX_INTEGER_ONLY int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* p = (const uint8_t*)a;
    const uint8_t* q = (const uint8_t*)b;

    /* two words a step: most compares here are of equal blocks, and run to the end */
    while (n >= 8) {
        if (((*(const Word*)p ^ *(const Word*)q) | (*(const Word*)(p + 4) ^ *(const Word*)(q + 4))) != 0) {
            break; /* the loop below finds the byte: the sign has to be the first difference's */
        }
        p += 8;
        q += 8;
        n -= 8;
    }
    for (; n != 0; n--, p++, q++) {
        if (*p != *q) {
            return (int)*p - (int)*q;
        }
    }
    return 0;
}

LUSX_INTEGER_ONLY void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;

    if (d == s || n == 0) {
        return dst;
    }
    if (d < s) {
        if ((size_t)(s - d) >= n) {
            return memcpy(dst, src, n); /* apart: memcpy doesn't promise an order, and needn't */
        }
        /* overlapping, the destination below: forwards, by words if a word's store can't reach what's unread */
        if ((size_t)(s - d) >= 4) {
            for (; n >= 4; n -= 4, d += 4, s += 4) {
                *(Word*)d = *(const Word*)s;
            }
        }
        for (; n != 0; n--) {
            *d++ = *s++;
        }
        return dst;
    }
    if ((size_t)(d - s) >= n) {
        return memcpy(dst, src, n);
    }
    /* overlapping, the destination above: backwards */
    d += n;
    s += n;
    if ((size_t)(d - s) >= 4) {
        for (; n >= 4; n -= 4) {
            d -= 4;
            s -= 4;
            *(Word*)d = *(const Word*)s;
        }
    }
    for (; n != 0; n--) {
        *--d = *--s;
    }
    return dst;
}

LUSX_INTEGER_ONLY void* memset(void* dst, int c, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint32_t v = (uint32_t)(uint8_t)c * 0x01010101u;

    if (n >= LUSX_REP_FROM) {
        /* to the 8 bytes the fast strings want, then a word at a time */
        for (; ((uintptr_t)d & 7) != 0; n--) {
            *d++ = (uint8_t)v;
        }
        size_t words = n >> 2;
        n &= 3;
        __asm__ volatile("rep stosl" : "+D"(d), "+c"(words) : "a"(v) : "memory");
    }
    for (; n >= 4; n -= 4, d += 4) {
        *(Word*)d = v;
    }
    for (; n != 0; n--) {
        *d++ = (uint8_t)v;
    }
    return dst;
}
