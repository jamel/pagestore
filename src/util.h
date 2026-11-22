#pragma once

/*
 * Shared assertions and byte helpers. The little-endian load and store
 * helpers work at any alignment, including fields inside disk headers.
 */

#include "pagestore/compiler.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

/* Page images go to disk in host byte order. */
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
_Static_assert(
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
    "pagestore requires a little-endian host");
#else
#error "pagestore cannot determine host byte order"
#endif

/* In release builds sizeof keeps the expression in scope without evaluating it. */
#ifdef NDEBUG
#define PS_ASSERT(x) ((void) sizeof((x) ? 1 : 0))
#else
#define PS_ASSERT(x) assert(x)
#endif

/* The chosen argument is evaluated twice. Pass values without side effects. */
#define PS_MIN(a, b) ((a) < (b) ? (a) : (b))
#define PS_MAX(a, b) ((a) > (b) ? (a) : (b))

static inline void ps_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) (v >> 8);
    p[2] = (uint8_t) (v >> 16);
    p[3] = (uint8_t) (v >> 24);
}

static inline void ps_put_le64(uint8_t *p, uint64_t v)
{
    ps_put_le32(p, (uint32_t) v);
    ps_put_le32(p + 4, (uint32_t) (v >> 32));
}

static inline uint32_t ps_get_le32(const uint8_t *p)
{
    return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

static inline uint64_t ps_get_le64(const uint8_t *p)
{
    return (uint64_t) ps_get_le32(p) | (uint64_t) ps_get_le32(p + 4) << 32;
}

static inline int ps_is_pow2(uint64_t v)
{
    return v != 0 && (v & (v - 1)) == 0;
}
