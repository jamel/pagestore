/*
 * Fold eight input bytes into the CRC per pass. memcpy accepts unaligned
 * buffers. On a little-endian host, the low byte is first in the input,
 * so its table accounts for the seven bytes that follow it.
 */

#include "crc32c.h"

#include "crc32c_tables.h"
#include "util.h"

#include <string.h>

uint32_t ps_crc32c(uint32_t crc, const void *buf, size_t len)
{
    const uint32_t (*tab)[256] = ps_crc32c_tab;
    const uint8_t *p = buf;

    crc = ~crc; /* Unwrap the public CRC to continue its running state. */
    while (len >= 8) {
        uint64_t w;
        memcpy(&w, p, 8);
        w ^= crc;
        crc = tab[7][w & 0xFF] ^ tab[6][(w >> 8) & 0xFF] ^ tab[5][(w >> 16) & 0xFF] ^
            tab[4][(w >> 24) & 0xFF] ^ tab[3][(w >> 32) & 0xFF] ^ tab[2][(w >> 40) & 0xFF] ^
            tab[1][(w >> 48) & 0xFF] ^ tab[0][w >> 56];
        p += 8;
        len -= 8;
    }
    while (len--) {
        crc = (crc >> 8) ^ tab[0][(crc ^ *p++) & 0xFF];
    }
    return ~crc;
}
