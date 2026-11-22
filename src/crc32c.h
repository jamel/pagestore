#ifndef PS_CRC32C_H
#define PS_CRC32C_H

/* CRC-32C with the Castagnoli polynomial. Calls from different threads can overlap. */

#include <stddef.h>
#include <stdint.h>

/*
 * CRC-32C (Castagnoli, reflected polynomial 0x82F63B78). Pass crc = 0 to
 * start, or a previous result to continue over the next chunk.
 * buf may be NULL when len is 0. Thread-safe.
 */
uint32_t ps_crc32c(uint32_t crc, const void *buf, size_t len);

#endif /* PS_CRC32C_H */
