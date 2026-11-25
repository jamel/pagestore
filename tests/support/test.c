#include "test.h"

#include <errno.h>
#include <ftw.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *t_prog = "test";
static const char *t_filter;
static const char *t_current;
static bool t_matched;
static int t_ran;

void t_init(int argc, char **argv)
{
    if (argc > 0 && argv[0] != NULL) {
        t_prog = argv[0];
    }
    if (argc > 2) {
        fprintf(stderr, "usage: %s [test_name]\n", t_prog);
        exit(2);
    }
    t_filter = argc > 1 ? argv[1] : NULL;
    /* Keep the last progress line visible if a worker aborts. */
    setvbuf(stdout, NULL, _IONBF, 0);
}

bool t_select(const char *name)
{
    if (t_filter != NULL && strcmp(t_filter, name) != 0) {
        return false;
    }
    t_matched = true;
    return true;
}

void t_begin(const char *name)
{
    t_current = name;
    printf("[ RUN  ] %s\n", name);
}

void t_end(const char *name)
{
    printf("[  OK  ] %s\n", name);
    t_current = NULL;
    t_ran++;
}

int t_done(void)
{
    if (t_filter != NULL && !t_matched) {
        fprintf(stderr, "%s: no test named '%s'\n", t_prog, t_filter);
        return 1;
    }
    printf("%d test(s) passed\n", t_ran);
    return 0;
}

static void t_where(const char *file, int line)
{
    fflush(stdout);
    fprintf(stderr, "%s:%d: ", file, line);
    if (t_current != NULL) {
        fprintf(stderr, "[%s] ", t_current);
    }
}

PS_NORETURN static void t_abort(void)
{
    fflush(stderr);
    abort();
}

void t_fail(const char *file, int line, const char *fmt, ...)
{
    va_list ap;

    t_where(file, line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    t_abort();
}

void t_fail_eq(
    const char *file,
    int line,
    const char *op,
    const char *as,
    const char *bs,
    unsigned long long a,
    unsigned long long b)
{
    t_where(file, line);
    fprintf(
        stderr,
        "%s(%s, %s) failed\n"
        "    left:  %llu (0x%llx, signed %lld)\n"
        "    right: %llu (0x%llx, signed %lld)\n",
        op,
        as,
        bs,
        a,
        a,
        (long long) a,
        b,
        b,
        (long long) b);
    t_abort();
}

void t_fail_err(const char *file, int line, const char *es, const char *cs, int got, int want)
{
    t_where(file, line);
    if (cs == NULL) {
        fprintf(stderr, "T_OK(%s) failed: %d (%s)\n", es, got, strerror(got));
    } else {
        fprintf(
            stderr,
            "T_ERR(%s, %s) failed: got %d (%s), want %d (%s)\n",
            es,
            cs,
            got,
            got == 0 ? "success" : strerror(got),
            want,
            strerror(want));
    }
    t_abort();
}

void t_fail_mem(
    const char *file,
    int line,
    const char *as,
    const char *bs,
    const void *a,
    const void *b,
    usize n)
{
    const unsigned char *pa = a, *pb = b;
    usize i = 0, diffs = 0;

    while (i < n && pa[i] == pb[i]) {
        i++;
    }
    for (usize j = i; j < n; j++) {
        diffs += pa[j] != pb[j];
    }
    t_where(file, line);
    fprintf(
        stderr,
        "T_MEMEQ(%s, %s, %zu) failed: first difference at offset %zu "
        "(0x%02x != 0x%02x), %zu byte(s) differ\n",
        as,
        bs,
        n,
        i,
        pa[i],
        pb[i],
        diffs);
    t_abort();
}

void *t_malloc(usize n)
{
    void *p = malloc(n != 0 ? n : 1);
    if (p == NULL) {
        T_FAIL("out of memory allocating %zu bytes", n);
    }
    return p;
}

void *t_calloc(usize count, usize size)
{
    void *p = calloc(count != 0 ? count : 1, size != 0 ? size : 1);
    if (p == NULL) {
        T_FAIL("out of memory allocating %zu x %zu bytes", count, size);
    }
    return p;
}

void *t_realloc(void *p, usize n)
{
    void *q = realloc(p, n != 0 ? n : 1);
    if (q == NULL) {
        T_FAIL("out of memory growing to %zu bytes", n);
    }
    return q;
}

u32 t_scale(void)
{
    const char *s = getenv("PS_TEST_SCALE");
    char *end;
    unsigned long v;

    if (s == NULL || *s == '\0') {
        return 1;
    }
    errno = 0;
    v = strtoul(s, &end, 10);
    const int rv = errno;
    if (rv != 0 || *end != '\0' || v == 0 || v > 1000000) {
        fprintf(stderr, "bad PS_TEST_SCALE '%s', using 1\n", s);
        return 1;
    }
    return (u32) v;
}

char *t_tmpdir(void)
{
    const char *base = getenv("TMPDIR");
    char *path;
    usize len;

    if (base == NULL || *base == '\0') {
        base = "/tmp";
    }
    len = strlen(base) + sizeof("/pstest.XXXXXX");
    path = t_malloc(len);
    snprintf(path, len, "%s/pstest.XXXXXX", base);
    if (mkdtemp(path) == NULL) {
        const int rv = errno;
        T_FAIL("t_tmpdir: mkdtemp(%s): %s", path, strerror(rv));
    }
    return path;
}

static int t_rm_cb(const char *path, const struct stat *sb, int flag, struct FTW *ftw)
{
    PS_UNUSED(sb, flag, ftw);
    if (remove(path) != 0) {
        const int rv = errno;
        if (rv != ENOENT) {
            fprintf(stderr, "t_rmtree: remove(%s): %s\n", path, strerror(rv));
            return -1;
        }
    }
    return 0;
}

static void t_rmtree(const char *path)
{
    if (access(path, F_OK) != 0) {
        const int rv = errno;
        if (rv == ENOENT) {
            return;
        }
    }
    if (nftw(path, t_rm_cb, 16, FTW_DEPTH | FTW_PHYS) != 0) {
        const int rv = errno;
        T_FAIL("t_rmtree(%s) failed: %s", path, strerror(rv));
    }
}

void t_tmpdir_free(char *dir)
{
    t_rmtree(dir);
    free(dir);
}

u64 t_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        const int rv = errno;
        T_FAIL("clock_gettime: %s", strerror(rv));
    }
    return (u64) ts.tv_sec * 1000u + (u64) ts.tv_nsec / 1000000u;
}

static void t_sleep_ns(u64 ns)
{
    struct timespec ts = {(time_t) (ns / 1000000000u), (long) (ns % 1000000000u)};
    while (nanosleep(&ts, &ts) != 0) {
        /* Interrupted: nanosleep left the remaining time in ts. */
    }
}

void t_sleep_ms(u32 ms)
{
    t_sleep_ns((u64) ms * 1000000u);
}

void t_sleep_us(u32 us)
{
    t_sleep_ns((u64) us * 1000u);
}
