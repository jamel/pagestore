/*
 * Fold eight input bytes into the CRC per pass. ps_get_le64 accepts
 * unaligned buffers and puts the first input byte in the low bits, so its
 * table accounts for the seven bytes that follow it.
 */

#include "crc32c.h"

#include "crc32c_tables.h"
#include "pagestore/bytes.h"

u32 ps_crc32c(u32 crc, const void *buf, usize len)
{
    const u32(*tab)[256] = ps_crc32c_tab;
    const u8 *p = buf;

    crc = ~crc; /* Unwrap the public CRC to continue its running state. */
    while (len >= 8) {
        const u64 w = ps_get_le64(p) ^ crc;
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
