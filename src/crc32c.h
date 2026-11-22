#pragma once

/* CRC-32C with the Castagnoli polynomial. Calls from different threads can overlap. */

#include "pagestore/types.h"

/*
 * Start with crc = 0. To checksum two chunks, call
 * ps_crc32c(ps_crc32c(0, a, na), b, nb). A zero-length chunk may use NULL.
 */
u32 ps_crc32c(u32 crc, const void *buf, usize len);
