#pragma once

/*
 * Shared assertions and byte helpers. The little-endian load and store
 * helpers work at any alignment, including fields inside disk headers.
 */

#include "pagestore/compiler.h"
#include "pagestore/types.h"

#include <assert.h>
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

static inline void ps_put_le32(u8 *p, u32 v)
{
    p[0] = (u8) v;
    p[1] = (u8) (v >> 8);
    p[2] = (u8) (v >> 16);
    p[3] = (u8) (v >> 24);
}

static inline void ps_put_le64(u8 *p, u64 v)
{
    ps_put_le32(p, (u32) v);
    ps_put_le32(p + 4, (u32) (v >> 32));
}

static inline u32 ps_get_le32(const u8 *p)
{
    return (u32) p[0] | (u32) p[1] << 8 | (u32) p[2] << 16 | (u32) p[3] << 24;
}

static inline u64 ps_get_le64(const u8 *p)
{
    return (u64) ps_get_le32(p) | (u64) ps_get_le32(p + 4) << 32;
}

static inline bool ps_is_pow2(u64 v)
{
    return v != 0 && (v & (v - 1)) == 0;
}
