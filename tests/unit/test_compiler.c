#include "pagestore/compiler.h"
#include "test.h"

/* These helpers fail -Werror=unused-parameter if PS_UNUSED misses an argument. */
static int one(int a)
{
    PS_UNUSED(a);
    return 1;
}

static int three(int a, const char *b, f64 c)
{
    PS_UNUSED(a, b, c);
    return 3;
}

static int eight(int a, int b, int c, int d, int e, int f, int g, int h)
{
    PS_UNUSED(a, b, c, d, e, f, g, h);
    return 8;
}

static void test_unused(void)
{
    T_EQ(one(0), 1);
    T_EQ(three(0, "x", 0.0), 3);
    T_EQ(eight(0, 0, 0, 0, 0, 0, 0, 0), 8);

    int n = 0;
    PS_UNUSED(n++, n++);
    T_EQ(n, 2);
}

PS_STATIC_ASSERT(sizeof(u32) == 4, "u32 is 4 bytes");

static void test_static_assert(void)
{
    PS_STATIC_ASSERT(sizeof(u64) == 8, "u64 is 8 bytes");
    T_EQ(sizeof(u64), 8);
}

static void test_bit_scans(void)
{
    T_EQ(PS_CTZ64(1ull), 0);
    T_EQ(PS_CTZ64(1ull << 63), 63);
    T_EQ(PS_POPCOUNT64(0ull), 0);
    T_EQ(PS_POPCOUNT64(UINT64_MAX), 64);
}

int main(int argc, char **argv)
{
    t_init(argc, argv);
    T_RUN(test_unused);
    T_RUN(test_static_assert);
    T_RUN(test_bit_scans);
    return t_done();
}
