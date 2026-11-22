#pragma once

/*
 * Use fixed-width types such as u32 for page data. Use usize for buffer
 * sizes and isize for pointer differences. Their width depends on the
 * host, so keep them out of the on-disk format.
 */

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

_Static_assert(sizeof(f32) == 4, "f32 must be 32 bits");
_Static_assert(sizeof(f64) == 8, "f64 must be 64 bits");
_Static_assert(sizeof(isize) == sizeof(usize), "isize and usize must have the same width");
