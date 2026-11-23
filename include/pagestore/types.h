#pragma once

/*
 * Use fixed-width types such as u32 for page data. Use usize for buffer
 * sizes and isize for pointer differences. Their width depends on the
 * host, so keep them out of the on-disk format.
 */

#include "pagestore/compiler.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef ptrdiff_t isize;
typedef size_t usize;

typedef float f32;
typedef double f64;

PS_STATIC_ASSERT(sizeof(f32) == 4, "f32 must be 32 bits");
PS_STATIC_ASSERT(sizeof(f64) == 8, "f64 must be 64 bits");
PS_STATIC_ASSERT(sizeof(isize) == sizeof(usize), "isize and usize must have the same width");

/*
 * A struct around one base value, so the compiler rejects mixing the two.
 * name_from wraps a value, name_eq compares, name_is_none tests for 0, and
 * .raw reads the value back.
 */
#define PS_NEWTYPE(name, base) \
    typedef struct name { \
        base raw; \
    } name; \
    PS_MAYBE_UNUSED static inline name name##_from(base raw) \
    { \
        const name v = {raw}; \
        return v; \
    } \
    PS_MAYBE_UNUSED static inline bool name##_eq(name a, name b) \
    { \
        return a.raw == b.raw; \
    } \
    PS_MAYBE_UNUSED static inline bool name##_is_none(name a) \
    { \
        return a.raw == 0; \
    } \
    PS_STATIC_ASSERT(sizeof(name) == sizeof(base), #name " must be the size of " #base)

/* The store never hands out PS_PAGE_ID_NONE, so layers on top can use it to mean "no page". */
PS_NEWTYPE(ps_page_id, u32);

#define PS_PAGE_ID_NONE PS_LITERAL(ps_page_id, 0)
