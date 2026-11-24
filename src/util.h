#pragma once

/*
 * Shared assertions and small helpers. Byte-order helpers and the
 * little-endian host check live in the public pagestore/bytes.h.
 */

#include "pagestore/compiler.h"
#include "pagestore/types.h"

#include <assert.h>
#include <string.h>

/* In release builds sizeof keeps the expression in scope without evaluating it. */
#ifdef NDEBUG
#define PS_ASSERT(x) ((void) sizeof((x) ? 1 : 0))
#else
#define PS_ASSERT(x) assert(x)
#endif

/* The chosen argument is evaluated twice. Pass values without side effects. */
#define PS_MIN(a, b) ((a) < (b) ? (a) : (b))
#define PS_MAX(a, b) ((a) > (b) ? (a) : (b))

static inline bool ps_is_pow2(u64 v)
{
    return v != 0 && (v & (v - 1)) == 0;
}
