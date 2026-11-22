/*
 * Scan bitmaps a word at a time. memcpy allows unaligned inputs, and
 * little-endian word order keeps bit numbers consistent with the byte
 * array. For a partial word, copy only the bytes inside the bitmap.
 */

#include "bitmap.h"

#include "pagestore/compiler.h"
#include "util.h"

#include <string.h>

void ps_bit_set(uint8_t *m, uint32_t bit)
{
    m[bit / 8] |= (uint8_t) (1u << (bit % 8));
}

void ps_bit_clear(uint8_t *m, uint32_t bit)
{
    m[bit / 8] &= (uint8_t) ~(1u << (bit % 8));
}

int ps_bit_test(const uint8_t *m, uint32_t bit)
{
    return (m[bit / 8] >> (bit % 8)) & 1;
}

/* Pad the last word with ones when scanning and zeros when counting. */
static uint64_t load_word(const uint8_t *m, uint32_t nbytes, uint32_t w, uint8_t fill)
{
    uint64_t v;
    uint32_t off = w * 8;
    uint32_t n = nbytes - off < 8 ? nbytes - off : 8;

    if (n == 8) {
        memcpy(&v, m + off, 8);
    } else {
        uint8_t b[8];
        memset(b, fill, sizeof(b));
        memcpy(b, m + off, n);
        memcpy(&v, b, 8);
    }
    return v;
}

uint32_t ps_bit_find_clear(const uint8_t *m, uint32_t nbits, uint32_t from)
{
    if (from >= nbits) {
        return nbits;
    }

    uint32_t nbytes = (uint32_t) (((uint64_t) nbits + 7) / 8);
    uint32_t nwords = (nbytes + 7) / 8;
    uint32_t w = from / 64;
    uint64_t x = ~load_word(m, nbytes, w, 0xFF) & (~0ull << (from % 64));

    for (;;) {
        if (x != 0) {
            uint64_t bit = (uint64_t) w * 64 + (uint64_t) PS_CTZ64(x);
            return bit < nbits ? (uint32_t) bit : nbits;
        }
        if (++w >= nwords) {
            return nbits;
        }
        x = ~load_word(m, nbytes, w, 0xFF);
    }
}

uint32_t ps_bit_count_set(const uint8_t *m, uint32_t nbits)
{
    uint32_t nbytes = (uint32_t) (((uint64_t) nbits + 7) / 8);
    uint32_t full = nbits / 64;
    uint32_t count = 0;

    for (uint32_t w = 0; w < full; w++) {
        count += PS_POPCOUNT64(load_word(m, nbytes, w, 0));
    }
    if (nbits % 64 != 0) {
        uint64_t x = load_word(m, nbytes, full, 0) & ((1ull << (nbits % 64)) - 1);
        count += PS_POPCOUNT64(x);
    }
    return count;
}
