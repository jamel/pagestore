#pragma once

/*
 * Register tests with T_RUN in main.
 *
 *     int main(int argc, char **argv) {
 *         t_init(argc, argv);
 *         T_RUN(test_a);
 *         T_RUN(test_b);
 *         return t_done();
 *     }
 *
 * Run "prog" for the whole suite or "prog test_b" for one case.
 * A failed check prints to stderr and aborts the process, even from a
 * worker thread.
 */

#include "pagestore/compiler.h"
#include "pagestore/types.h"

void t_init(int argc, char **argv);
int t_done(void); /* Returns 1 if the requested test name matched nothing. */

bool t_select(const char *name);
void t_begin(const char *name);
void t_end(const char *name);

#define T_RUN(fn) \
    do { \
        if (t_select(#fn)) { \
            t_begin(#fn); \
            fn(); \
            t_end(#fn); \
        } \
    } while (0)

PS_NORETURN void t_fail(const char *file, int line, const char *fmt, ...) PS_PRINTF(3, 4);
PS_NORETURN void t_fail_eq(
    const char *file,
    int line,
    const char *op,
    const char *as,
    const char *bs,
    unsigned long long a,
    unsigned long long b);
PS_NORETURN void t_fail_err(
    const char *file,
    int line,
    const char *es,
    const char *cs,
    int got,
    int want);
PS_NORETURN void t_fail_mem(
    const char *file,
    int line,
    const char *as,
    const char *bs,
    const void *a,
    const void *b,
    usize n);

#define T_CHECK(c) \
    do { \
        if (!(c)) \
            t_fail(__FILE__, __LINE__, "T_CHECK(%s) failed", #c); \
    } while (0)

#define T_EQ(a, b) \
    do { \
        unsigned long long t_a_ = (unsigned long long) (a); \
        unsigned long long t_b_ = (unsigned long long) (b); \
        if (t_a_ != t_b_) \
            t_fail_eq(__FILE__, __LINE__, "T_EQ", #a, #b, t_a_, t_b_); \
    } while (0)

#define T_NE(a, b) \
    do { \
        unsigned long long t_a_ = (unsigned long long) (a); \
        unsigned long long t_b_ = (unsigned long long) (b); \
        if (t_a_ == t_b_) \
            t_fail_eq(__FILE__, __LINE__, "T_NE", #a, #b, t_a_, t_b_); \
    } while (0)

#define T_OK(e) \
    do { \
        int t_e_ = (e); \
        if (t_e_ != 0) \
            t_fail_err(__FILE__, __LINE__, #e, NULL, t_e_, 0); \
    } while (0)

/* Pass the error value returned directly by the call. */
#define T_ERR(e, code) \
    do { \
        int t_e_ = (e); \
        int t_c_ = (code); \
        if (t_e_ != t_c_) \
            t_fail_err(__FILE__, __LINE__, #e, #code, t_e_, t_c_); \
    } while (0)

#define T_MEMEQ(a, b, n) \
    do { \
        const void *t_a_ = (a); \
        const void *t_b_ = (b); \
        usize t_n_ = (n); \
        if (t_n_ != 0 && memcmp(t_a_, t_b_, t_n_) != 0) \
            t_fail_mem(__FILE__, __LINE__, #a, #b, t_a_, t_b_, t_n_); \
    } while (0)

#include <string.h>

/* Reads PS_TEST_SCALE. Values outside 1..1000000 fall back to 1. */
u32 t_scale(void);

/*
 * Creates a directory under TMPDIR, or /tmp by default. Call t_rmtree
 * on the returned path and then free it.
 */
char *t_tmpdir(void);

/* A missing path is fine. Removal errors abort the test. */
void t_rmtree(const char *path);

/* Monotonic clock in milliseconds. */
u64 t_now_ms(void);
