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
static const char *t_filter;  /* argv[1], or NULL to run everything */
static const char *t_current; /* name of the running test */
static int t_matched;
static int t_ran;

void t_init(int argc, char **argv)
{
    if (argc > 0 && argv[0] != NULL)
        t_prog = argv[0];
    if (argc > 2) {
        fprintf(stderr, "usage: %s [test_name]\n", t_prog);
        exit(2);
    }
    t_filter = argc > 1 ? argv[1] : NULL;
    /* Keep the last progress line visible if a worker aborts. */
    setvbuf(stdout, NULL, _IONBF, 0);
}

int t_select(const char *name)
{
    if (t_filter != NULL && strcmp(t_filter, name) != 0)
        return 0;
    t_matched = 1;
    return 1;
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
    if (t_current != NULL)
        fprintf(stderr, "[%s] ", t_current);
}

_Noreturn static void t_abort(void)
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

void t_fail_eq(const char *file, int line, const char *op, const char *as, const char *bs,
               unsigned long long a, unsigned long long b)
{
    t_where(file, line);
    fprintf(stderr,
            "%s(%s, %s) failed\n"
            "    left:  %llu (0x%llx, signed %lld)\n"
            "    right: %llu (0x%llx, signed %lld)\n",
            op, as, bs, a, a, (long long)a, b, b, (long long)b);
    t_abort();
}

void t_fail_err(const char *file, int line, const char *es, const char *cs, int got, int want)
{
    t_where(file, line);
    if (cs == NULL)
        fprintf(stderr, "T_OK(%s) failed: %d (%s)\n", es, got, strerror(got));
    else
        fprintf(stderr, "T_ERR(%s, %s) failed: got %d (%s), want %d (%s)\n", es, cs, got,
                got == 0 ? "success" : strerror(got), want, strerror(want));
    t_abort();
}

void t_fail_mem(const char *file, int line, const char *as, const char *bs, const void *a,
                const void *b, size_t n)
{
    const unsigned char *pa = a, *pb = b;
    size_t i = 0, diffs = 0;

    while (i < n && pa[i] == pb[i])
        i++;
    for (size_t j = i; j < n; j++)
        diffs += pa[j] != pb[j];
    t_where(file, line);
    fprintf(stderr,
            "T_MEMEQ(%s, %s, %zu) failed: first difference at offset %zu "
            "(0x%02x != 0x%02x), %zu byte(s) differ\n",
            as, bs, n, i, pa[i], pb[i], diffs);
    t_abort();
}

unsigned t_scale(void)
{
    const char *s = getenv("PS_TEST_SCALE");
    char *end;
    unsigned long v;

    if (s == NULL || *s == '\0')
        return 1;
    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno != 0 || *end != '\0' || v == 0 || v > 1000000) {
        fprintf(stderr, "bad PS_TEST_SCALE '%s', using 1\n", s);
        return 1;
    }
    return (unsigned)v;
}

char *t_tmpdir(void)
{
    const char *base = getenv("TMPDIR");
    char *path;
    size_t len;

    if (base == NULL || *base == '\0')
        base = "/tmp";
    len = strlen(base) + sizeof("/pstest.XXXXXX");
    path = malloc(len);
    if (path == NULL)
        t_fail(__FILE__, __LINE__, "t_tmpdir: out of memory");
    snprintf(path, len, "%s/pstest.XXXXXX", base);
    if (mkdtemp(path) == NULL)
        t_fail(__FILE__, __LINE__, "t_tmpdir: mkdtemp(%s): %s", path, strerror(errno));
    return path;
}

static int t_rm_cb(const char *path, const struct stat *sb, int flag, struct FTW *ftw)
{
    (void)sb;
    (void)flag;
    (void)ftw;
    if (remove(path) != 0 && errno != ENOENT) {
        fprintf(stderr, "t_rmtree: remove(%s): %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

void t_rmtree(const char *path)
{
    if (access(path, F_OK) != 0 && errno == ENOENT)
        return;
    if (nftw(path, t_rm_cb, 16, FTW_DEPTH | FTW_PHYS) != 0)
        t_fail(__FILE__, __LINE__, "t_rmtree(%s) failed: %s", path, strerror(errno));
}

uint64_t t_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        t_fail(__FILE__, __LINE__, "clock_gettime: %s", strerror(errno));
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
