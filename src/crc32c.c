/*
 * Fold eight input bytes into the CRC per pass. memcpy accepts unaligned
 * buffers. On a little-endian host, the low byte is first in the input,
 * so its table accounts for the seven bytes that follow it.
 */

#include "crc32c.h"

#include "util.h"

#include <pthread.h>
#include <string.h>

#define CRC32C_POLY 0x82F63B78u

/* Slicing-by-8 tables: tab[k][b] is the CRC of byte b followed by k zero bytes. */
static uint32_t tab[8][256];
static pthread_once_t tab_once = PTHREAD_ONCE_INIT;

static void tab_init(void)
{
    for (uint32_t b = 0; b < 256; b++) {
        uint32_t c = b;
        for (int i = 0; i < 8; i++)
            c = (c >> 1) ^ (CRC32C_POLY & (0u - (c & 1u)));
        tab[0][b] = c;
    }
    for (uint32_t b = 0; b < 256; b++) {
        uint32_t c = tab[0][b];
        for (int k = 1; k < 8; k++) {
            c = (c >> 8) ^ tab[0][c & 0xFF];
            tab[k][b] = c;
        }
    }
}

uint32_t ps_crc32c(uint32_t crc, const void *buf, size_t len)
{
    const uint8_t *p = buf;

    (void)pthread_once(&tab_once, tab_init);
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
    while (len--)
        crc = (crc >> 8) ^ tab[0][(crc ^ *p++) & 0xFF];
    return ~crc;
}
