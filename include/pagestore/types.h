#pragma once

/*
 * Use fixed-width types such as u32 for page data. Use usize for buffer
 * sizes and isize for pointer differences. Their width depends on the
 * host, so keep them out of the on-disk format.
 */

#include "pagestore/compiler.h" // IWYU pragma: export

#include <stdbool.h> // IWYU pragma: export
#include <stddef.h> // IWYU pragma: export
#include <stdint.h> // IWYU pragma: export

/*
 * i64 and u64 are long long so they print with %lld and %llu without a
 * cast. int64_t is long on some hosts and long long on others.
 */
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef long long i64;

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef unsigned long long u64;

typedef ptrdiff_t isize;
typedef size_t usize;

typedef float f32;
typedef double f64;

PS_STATIC_ASSERT(sizeof(i64) == 8, "i64 must be 64 bits");
PS_STATIC_ASSERT(sizeof(u64) == 8, "u64 must be 64 bits");
PS_STATIC_ASSERT(sizeof(f32) == 4, "f32 must be 32 bits");
PS_STATIC_ASSERT(sizeof(f64) == 8, "f64 must be 64 bits");
PS_STATIC_ASSERT(sizeof(isize) == sizeof(usize), "isize and usize must have the same width");

/*
 * A struct around one integer value, so the compiler rejects mixing the
 * two. name_from wraps a value and .raw reads it back. name_eq, name_lt,
 * name_le, name_gt and name_ge compare, and name_is_none tests for 0.
 * name_next adds one, and the caller must make sure it cannot overflow.
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
    PS_MAYBE_UNUSED static inline bool name##_lt(name a, name b) \
    { \
        return a.raw < b.raw; \
    } \
    PS_MAYBE_UNUSED static inline bool name##_le(name a, name b) \
    { \
        return a.raw <= b.raw; \
    } \
    PS_MAYBE_UNUSED static inline bool name##_gt(name a, name b) \
    { \
        return a.raw > b.raw; \
    } \
    PS_MAYBE_UNUSED static inline bool name##_ge(name a, name b) \
    { \
        return a.raw >= b.raw; \
    } \
    PS_MAYBE_UNUSED static inline bool name##_is_none(name a) \
    { \
        return a.raw == 0; \
    } \
    PS_MAYBE_UNUSED static inline name name##_next(name a) \
    { \
        return name##_from((base) (a.raw + 1)); \
    } \
    PS_STATIC_ASSERT(sizeof(name) == sizeof(base), #name " must be the size of " #base)

/* A page's index in the store. Page 0 is a bitmap, so PS_PAGE_ID_NONE can mean "no page". */
PS_NEWTYPE(ps_page_id, u32);
#define PS_PAGE_ID_NONE PS_LITERAL(ps_page_id, 0)

/* A log sequence number (LSN) orders operations. A new store starts at PS_LSN_NONE. */
PS_NEWTYPE(ps_lsn, u64);
#define PS_LSN_NONE PS_LITERAL(ps_lsn, 0)
#define PS_LSN_MAX PS_LITERAL(ps_lsn, UINT64_MAX)

typedef struct ps_store ps_store;
