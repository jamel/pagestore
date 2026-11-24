#include "crc32c.h"
#include "test.h"

#include "pagestore/types.h"

#include <stdlib.h>
#include <string.h>

/* The standard CRC-32C check value uses the bytes "123456789". */
static void test_check_value(void)
{
    T_EQ(ps_crc32c(0, "123456789", 9), 0xE3069283u);
}

/* RFC 3720 (iSCSI), Appendix B.4. */
static void test_rfc3720_vectors(void)
{
    u8 b[32];

    memset(b, 0, sizeof(b));
    T_EQ(ps_crc32c(0, b, sizeof(b)), 0x8A9136AAu);

    memset(b, 0xFF, sizeof(b));
    T_EQ(ps_crc32c(0, b, sizeof(b)), 0x62A8AB43u);

    for (int i = 0; i < 32; i++) {
        b[i] = (u8) i;
    }
    T_EQ(ps_crc32c(0, b, sizeof(b)), 0x46DD794Eu);

    for (int i = 0; i < 32; i++) {
        b[i] = (u8) (31 - i);
    }
    T_EQ(ps_crc32c(0, b, sizeof(b)), 0x113FDB5Cu);
}

static void test_incremental_and_empty(void)
{
    T_EQ(ps_crc32c(ps_crc32c(0, "1234", 4), "56789", 5), 0xE3069283u);
    T_EQ(ps_crc32c(0, "", 0), 0u);
    T_EQ(ps_crc32c(0, NULL, 0), 0u);
    T_EQ(ps_crc32c(0xE3069283u, "", 0), 0xE3069283u);
}

/* Compute one bit at a time to test the generated tables independently. */
static u32 crc32c_bitwise(const u8 *p, usize len)
{
    u32 crc = 0xFFFFFFFFu;
    for (usize i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static void fill_pattern(u8 *p, usize len)
{
    u32 x = 0x9E3779B9u;
    for (usize i = 0; i < len; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        p[i] = (u8) x;
    }
}

/* Compare each offset with an aligned copy and the bitwise reference. */
static void test_unaligned_input(void)
{
    enum {
        N = 1000,
        PAD = 16
    };
    u8 *raw = malloc(N + PAD);
    u64 aligned_store[(N + 7) / 8];
    u8 *aligned = (u8 *) aligned_store;
    T_CHECK(raw != NULL);

    fill_pattern(raw, N + PAD);
    for (usize off = 1; off < 8; off++) {
        const u8 *src = raw + off;
        memcpy(aligned, src, N);
        u32 want = crc32c_bitwise(aligned, N);
        T_EQ(ps_crc32c(0, aligned, N), want);
        T_EQ(ps_crc32c(0, src, N), want);
    }
    for (usize len = 0; len <= N; len++) {
        const u8 *src = raw + 1 + (len % 7);
        memcpy(aligned, src, len);
        T_EQ(ps_crc32c(0, src, len), ps_crc32c(0, aligned, len));
        T_EQ(ps_crc32c(0, src, len), crc32c_bitwise(src, len));
    }
    free(raw);
}

/* Splitting the input at any point gives the same result as one call. */
static void test_split_points(void)
{
    u8 b[300];
    fill_pattern(b, sizeof(b));
    u32 whole = ps_crc32c(0, b, sizeof(b));
    for (usize cut = 0; cut <= sizeof(b); cut++) {
        T_EQ(ps_crc32c(ps_crc32c(0, b, cut), b + cut, sizeof(b) - cut), whole);
    }
}

int main(int argc, char **argv)
{
    t_init(argc, argv);
    T_RUN(test_check_value);
    T_RUN(test_rfc3720_vectors);
    T_RUN(test_incremental_and_empty);
    T_RUN(test_unaligned_input);
    T_RUN(test_split_points);
    return t_done();
}
