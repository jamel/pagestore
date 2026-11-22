#pragma once

/*
 * Bits are numbered from the low bit in each byte. For example, a
 * byte of 0x05 has bits 0 and 2 set. Calls that take nbits need only
 * ceil(nbits / 8) bytes, and ignore unused bits in the last byte.
 */

#include "pagestore/types.h"

void ps_bit_set(u8 *m, u32 bit);
void ps_bit_clear(u8 *m, u32 bit);
bool ps_bit_test(const u8 *m, u32 bit);

/* Finds the first clear bit in [from, nbits), or returns nbits. */
u32 ps_bit_find_clear(const u8 *m, u32 nbits, u32 from);

/* The number of set bits below nbits. */
u32 ps_bit_count_set(const u8 *m, u32 nbits);
