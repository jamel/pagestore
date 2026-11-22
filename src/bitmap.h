#pragma once

/*
 * Bits are numbered from the low bit in each byte. For example, a
 * byte of 0x05 has bits 0 and 2 set. Calls that take nbits need only
 * ceil(nbits / 8) bytes, and ignore unused bits in the last byte.
 */

#include <stdint.h>

void ps_bit_set(uint8_t *m, uint32_t bit);
void ps_bit_clear(uint8_t *m, uint32_t bit);
int ps_bit_test(const uint8_t *m, uint32_t bit); /* 0 or 1 */

/* Finds the first clear bit in [from, nbits), or returns nbits. */
uint32_t ps_bit_find_clear(const uint8_t *m, uint32_t nbits, uint32_t from);

uint32_t ps_bit_count_set(const uint8_t *m, uint32_t nbits);
