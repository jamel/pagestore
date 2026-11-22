/*
 * Scan bitmaps a word at a time. memcpy allows unaligned inputs, and
 * little-endian word order keeps bit numbers consistent with the byte
 * array. For a partial word, copy only the bytes inside the bitmap.
 */

#include "bitmap.h"

#include "pagestore/compiler.h"
#include "util.h"

#include <string.h>

void ps_bit_set(u8 *m, u32 bit)
{
    m[bit / 8] |= (u8) (1u << (bit % 8));
}

void ps_bit_clear(u8 *m, u32 bit)
{
    m[bit / 8] &= (u8) ~(1u << (bit % 8));
}

bool ps_bit_test(const u8 *m, u32 bit)
{
    return ((m[bit / 8] >> (bit % 8)) & 1) != 0;
}

/* Pad the last word with ones when scanning and zeros when counting. */
static u64 load_word(const u8 *m, u32 nbytes, u32 w, u8 fill)
{
    u64 v;
    u32 off = w * 8;
    u32 n = nbytes - off < 8 ? nbytes - off : 8;

    if (n == 8) {
        memcpy(&v, m + off, 8);
    } else {
        u8 b[8];
        memset(b, fill, sizeof(b));
        memcpy(b, m + off, n);
        memcpy(&v, b, 8);
    }
    return v;
}

u32 ps_bit_find_clear(const u8 *m, u32 nbits, u32 from)
{
    if (from >= nbits) {
        return nbits;
    }

    u32 nbytes = (u32) (((u64) nbits + 7) / 8);
    u32 nwords = (nbytes + 7) / 8;
    u32 w = from / 64;
    /* Ignore bits before from in the first word. */
    u64 x = ~load_word(m, nbytes, w, 0xFF) & (~0ull << (from % 64));

    for (;;) {
        if (x != 0) {
            u64 bit = (u64) w * 64 + (u64) PS_CTZ64(x);
            return bit < nbits ? (u32) bit : nbits;
        }
        if (++w >= nwords) {
            return nbits;
        }
        x = ~load_word(m, nbytes, w, 0xFF);
    }
}

u32 ps_bit_count_set(const u8 *m, u32 nbits)
{
    u32 nbytes = (u32) (((u64) nbits + 7) / 8);
    u32 full = nbits / 64;
    u32 count = 0;

    for (u32 w = 0; w < full; w++) {
        count += PS_POPCOUNT64(load_word(m, nbytes, w, 0));
    }
    if (nbits % 64 != 0) {
        u64 x = load_word(m, nbytes, full, 0) & ((1ull << (nbits % 64)) - 1);
        count += PS_POPCOUNT64(x);
    }
    return count;
}
