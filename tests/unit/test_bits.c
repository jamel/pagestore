#include "bitmap.h"
#include "test.h"
#include "util.h"

#include <stdint.h>
#include <string.h>

static void test_set_clear(void)
{
    uint8_t m[16] = {0};

    ps_bit_set(m, 0);
    ps_bit_set(m, 9);
    T_EQ(m[0], 0x01);
    T_EQ(m[1], 0x02);
    T_EQ(ps_bit_test(m, 0), 1);
    T_EQ(ps_bit_test(m, 9), 1);
    T_EQ(ps_bit_test(m, 8), 0);

    ps_bit_clear(m, 9);
    T_EQ(m[1], 0);
    T_EQ(ps_bit_test(m, 9), 0);

    ps_bit_set(m, 127);
    T_EQ(m[15], 0x80);
    ps_bit_set(m, 127); /* idempotent */
    T_EQ(m[15], 0x80);
    ps_bit_clear(m, 127);
    ps_bit_clear(m, 127);
    T_EQ(m[15], 0);
    T_EQ(m[0], 0x01);
}

static void test_find_clear(void)
{
    uint8_t m[16];

    memset(m, 0xFF, sizeof(m));
    ps_bit_clear(m, 70);
    ps_bit_clear(m, 127);
    T_EQ(ps_bit_find_clear(m, 128, 0), 70);
    T_EQ(ps_bit_find_clear(m, 128, 70), 70);
    T_EQ(ps_bit_find_clear(m, 128, 71), 127);
    T_EQ(ps_bit_find_clear(m, 127, 71), 127); /* same index now means no match */
    T_EQ(ps_bit_find_clear(m, 128, 128), 128);
    T_EQ(ps_bit_find_clear(m, 128, 1000), 128);
    T_EQ(ps_bit_find_clear(m, 0, 0), 0);

    memset(m, 0, sizeof(m));
    T_EQ(ps_bit_find_clear(m, 128, 0), 0);
    T_EQ(ps_bit_find_clear(m, 128, 63), 63);
    T_EQ(ps_bit_find_clear(m, 128, 64), 64);
    T_EQ(ps_bit_find_clear(m, 128, 127), 127);
}

/* A full-word read of this three-byte bitmap would overrun it. */
static void test_find_clear_partial(void)
{
    uint8_t m[3];

    memset(m, 0xFF, sizeof(m));
    T_EQ(ps_bit_find_clear(m, 20, 0), 20);
    ps_bit_clear(m, 19);
    T_EQ(ps_bit_find_clear(m, 20, 0), 19);
    T_EQ(ps_bit_find_clear(m, 19, 0), 19);
    ps_bit_clear(m, 5);
    T_EQ(ps_bit_find_clear(m, 20, 0), 5);
    T_EQ(ps_bit_find_clear(m, 20, 6), 19);
    T_EQ(ps_bit_find_clear(m, 5, 0), 5);

    uint8_t one = 0xFF;
    T_EQ(ps_bit_find_clear(&one, 8, 0), 8);
    one = 0x7F;
    T_EQ(ps_bit_find_clear(&one, 8, 0), 7);
    T_EQ(ps_bit_find_clear(&one, 7, 0), 7);
}

/* Use a bit-by-bit scan as the reference for the word search. */
static void test_find_clear_exhaustive(void)
{
    uint8_t m[40];
    uint32_t x = 12345;

    for (int round = 0; round < 50; round++) {
        for (size_t i = 0; i < sizeof(m); i++) {
            x = x * 1103515245u + 12345u;
            /* Long runs of set bits exercise scans that skip full words. */
            m[i] = (x >> 16) % 4 ? 0xFF : (uint8_t) (x >> 8);
        }
        for (uint32_t nbits = 0; nbits <= sizeof(m) * 8; nbits += 1 + nbits % 13) {
            for (uint32_t from = 0; from <= nbits + 1; from++) {
                uint32_t want = from;
                while (want < nbits && ps_bit_test(m, want)) {
                    want++;
                }
                if (want > nbits) {
                    want = nbits;
                }
                T_EQ(ps_bit_find_clear(m, nbits, from), want);
            }
        }
    }
}

static void test_count_set(void)
{
    uint8_t m[16] = {0};

    T_EQ(ps_bit_count_set(m, 128), 0);
    m[0] = 0xFF;
    m[15] = 0x80;
    T_EQ(ps_bit_count_set(m, 128), 9);
    T_EQ(ps_bit_count_set(m, 127), 8);
    T_EQ(ps_bit_count_set(m, 8), 8);
    T_EQ(ps_bit_count_set(m, 3), 3);
    T_EQ(ps_bit_count_set(m, 0), 0);

    memset(m, 0xFF, sizeof(m));
    T_EQ(ps_bit_count_set(m, 128), 128);
    T_EQ(ps_bit_count_set(m, 100), 100);
    T_EQ(ps_bit_count_set(m, 65), 65);
    T_EQ(ps_bit_count_set(m, 64), 64);
    T_EQ(ps_bit_count_set(m, 63), 63);
}

static void test_le_helpers(void)
{
    uint8_t b[9];

    ps_put_le32(b, 0x11223344u);
    T_EQ(b[0], 0x44);
    T_EQ(b[1], 0x33);
    T_EQ(b[2], 0x22);
    T_EQ(b[3], 0x11);
    T_EQ(ps_get_le32(b), 0x11223344u);

    ps_put_le64(b, 0x0102030405060708ull);
    T_EQ(b[0], 0x08);
    T_EQ(b[7], 0x01);
    T_EQ(ps_get_le64(b), 0x0102030405060708ull);

    ps_put_le64(b + 1, 0xFEDCBA9876543210ull);
    T_EQ(b[1], 0x10);
    T_EQ(ps_get_le64(b + 1), 0xFEDCBA9876543210ull);
    ps_put_le32(b + 1, 0xFFFFFFFEu);
    T_EQ(ps_get_le32(b + 1), 0xFFFFFFFEu);
}

static void test_misc_helpers(void)
{
    T_EQ(ps_is_pow2(0), 0);
    T_EQ(ps_is_pow2(1), 1);
    T_EQ(ps_is_pow2(4096), 1);
    T_EQ(ps_is_pow2(4097), 0);
    T_EQ(ps_is_pow2(65536), 1);
    T_EQ(ps_is_pow2(1ull << 63), 1);
    T_EQ(ps_is_pow2((1ull << 63) + 1), 0);
    T_EQ(ps_is_pow2(UINT64_MAX), 0);

    T_EQ(PS_MIN(3, 7), 3);
    T_EQ(PS_MAX(3, 7), 7);
    T_EQ(PS_MIN(7u, 7u), 7u);
}

int main(int argc, char **argv)
{
    t_init(argc, argv);
    T_RUN(test_set_clear);
    T_RUN(test_find_clear);
    T_RUN(test_find_clear_partial);
    T_RUN(test_find_clear_exhaustive);
    T_RUN(test_count_set);
    T_RUN(test_le_helpers);
    T_RUN(test_misc_helpers);
    return t_done();
}
