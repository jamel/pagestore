#pragma once

/*
 * Keep compiler extensions and C/C++ differences here. Wrap exported C
 * declarations in PS_EXTERN_C_BEGIN and PS_EXTERN_C_END so C++ callers
 * link to the same symbols.
 */

#ifdef __cplusplus
#define PS_EXTERN_C_BEGIN extern "C" {
#define PS_EXTERN_C_END }
#else
#define PS_EXTERN_C_BEGIN
#define PS_EXTERN_C_END
#endif

#if !defined(__GNUC__) && !defined(__clang__)
#error "pagestore requires gcc or clang"
#endif

#ifdef __cplusplus
#define PS_NORETURN [[noreturn]]
#else
#define PS_NORETURN _Noreturn
#endif

/* Works at file scope and inside a function. */
#ifdef __cplusplus
#define PS_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define PS_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

/* In void report(int level, const char *fmt, ...), use PS_PRINTF(2, 3). */
#define PS_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))

#define PS_LIKELY(x) __builtin_expect(!!(x), 1)
#define PS_UNLIKELY(x) __builtin_expect(!!(x), 0)

/* PS_CTZ64 requires a nonzero argument. */
#define PS_CTZ64(x) ((u32) __builtin_ctzll(x))
#define PS_POPCOUNT64(x) ((u32) __builtin_popcountll(x))

/* PS_UNUSED accepts one to eight arguments and evaluates each once. */
#define PS_UNUSED_1(a) ((void) (a))
#define PS_UNUSED_2(a, ...) ((void) (a), PS_UNUSED_1(__VA_ARGS__))
#define PS_UNUSED_3(a, ...) ((void) (a), PS_UNUSED_2(__VA_ARGS__))
#define PS_UNUSED_4(a, ...) ((void) (a), PS_UNUSED_3(__VA_ARGS__))
#define PS_UNUSED_5(a, ...) ((void) (a), PS_UNUSED_4(__VA_ARGS__))
#define PS_UNUSED_6(a, ...) ((void) (a), PS_UNUSED_5(__VA_ARGS__))
#define PS_UNUSED_7(a, ...) ((void) (a), PS_UNUSED_6(__VA_ARGS__))
#define PS_UNUSED_8(a, ...) ((void) (a), PS_UNUSED_7(__VA_ARGS__))
#define PS_UNUSED_IMPLS \
    PS_UNUSED_8, PS_UNUSED_7, PS_UNUSED_6, PS_UNUSED_5, PS_UNUSED_4, PS_UNUSED_3, PS_UNUSED_2, \
        PS_UNUSED_1
#define PS_UNUSED_PICK(_1, _2, _3, _4, _5, _6, _7, _8, name, ...) name
#define PS_UNUSED_PICK_(...) PS_UNUSED_PICK(__VA_ARGS__)
#define PS_UNUSED(...) PS_UNUSED_PICK_(__VA_ARGS__, PS_UNUSED_IMPLS, )(__VA_ARGS__)
