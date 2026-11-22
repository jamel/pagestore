#pragma once

/* CRC-32C with the Castagnoli polynomial. Calls from different threads can overlap. */

#include <stddef.h>
#include <stdint.h>

/*
 * Start with crc = 0. To checksum two chunks, call
 * ps_crc32c(ps_crc32c(0, a, na), b, nb). A zero-length chunk may use NULL.
 */
uint32_t ps_crc32c(uint32_t crc, const void *buf, size_t len);
