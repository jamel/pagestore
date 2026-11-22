#pragma once

/* xoshiro256** with splitmix64 to expand the seed into four words. */

#include "pagestore/types.h"

typedef struct rng {
    u64 s[4];
} rng;

static inline u64 rng_splitmix64_(u64 *x)
{
    u64 z = (*x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static inline void rng_seed(rng *r, u64 seed)
{
    u64 x = seed;
    for (usize i = 0; i < 4; i++) {
        r->s[i] = rng_splitmix64_(&x);
    }
}

static inline u64 rng_rotl_(u64 v, u32 k)
{
    return (v << k) | (v >> (64 - k));
}

static inline u64 rng_next(rng *r)
{
    u64 *s = r->s;
    const u64 result = rng_rotl_(s[1] * 5, 7) * 9;
    const u64 t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rng_rotl_(s[3], 45);
    return result;
}

/* Uniform in [0, n), with 0 for an empty range. */
static inline u64 rng_below(rng *r, u64 n)
{
    if (n == 0) {
        return 0;
    }
    /* Drop 2^64 mod n values to leave a whole number of buckets. */
    const u64 threshold = (0 - n) % n;
    for (;;) {
        const u64 x = rng_next(r);
        if (x >= threshold) {
            return x % n;
        }
    }
}
