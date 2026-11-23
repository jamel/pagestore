#include "frames.h"
#include "test.h"
#include "util.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

/* Only the pointer identity matters to the frame pool. */
struct ps_vnode {
    int tag;
};

static void test_create_limits(void)
{
    ps_frames *fp = NULL;

    T_OK(ps_frames_create(4096, 16 * 4096, NULL, &fp));
    T_EQ(ps_frames_count(fp), 16);
    T_EQ(ps_frames_free_count(fp), 16);
    T_EQ(ps_frames_evictable_count(fp), 0);
    ps_frames_destroy(fp);

    T_OK(ps_frames_create(4096, 17 * 4096 + 100, NULL, &fp));
    T_EQ(ps_frames_count(fp), 17);
    ps_frames_destroy(fp);

    T_OK(ps_frames_create(8192, 16 * 8192, NULL, &fp));
    T_EQ(ps_frames_count(fp), 16);
    T_EQ((uintptr_t) ps_frame(fp, 0) % 4096, 0);
    ps_frames_destroy(fp);

    fp = NULL;
    T_ERR(ps_frames_create(4096, 15 * 4096, NULL, &fp), EINVAL);
    T_ERR(ps_frames_create(4096, 16 * 4096 - 1, NULL, &fp), EINVAL);
    T_ERR(ps_frames_create(4096, 0, NULL, &fp), EINVAL);
    T_ERR(ps_frames_create(0, 16 * 4096, NULL, &fp), EINVAL);
    T_ERR(ps_frames_create(4097, 16 * 4097, NULL, &fp), EINVAL);
    T_CHECK(fp == NULL);

    void *mem = NULL;
    T_EQ(posix_memalign(&mem, 4096, 16 * 4096 + 4096), 0);
    T_ERR(ps_frames_create(4096, 16 * 4096, (u8 *) mem + 8, &fp), EINVAL);
    T_CHECK(fp == NULL);
    T_OK(ps_frames_create(4096, 16 * 4096, mem, &fp));
    T_EQ(ps_frames_count(fp), 16);
    T_CHECK(ps_frame(fp, 0) == mem);
    T_CHECK(ps_frame(fp, 15) == (u8 *) mem + 15 * 4096);
    ps_frames_destroy(fp);
    free(mem);
}

static void test_alloc_release(void)
{
    ps_frames *fp = NULL;
    u32 f[16];
    bool seen[16] = {false};

    T_OK(ps_frames_create(4096, 16 * 4096, NULL, &fp));
    u8 *base = ps_frame(fp, 0);
    T_EQ((uintptr_t) base % 4096, 0);
    for (u32 i = 0; i < 16; i++) {
        T_OK(ps_frames_alloc(fp, &f[i]));
        T_CHECK(f[i] < 16);
        T_CHECK(!seen[f[i]]);
        seen[f[i]] = true;
        u8 *p = ps_frame(fp, f[i]);
        T_EQ((uintptr_t) p % 4096, 0);
        T_EQ((usize) (p - base), (usize) f[i] * 4096);
        memset(p, (int) i, 4096);
        T_EQ(ps_frames_free_count(fp), 15 - i);
    }
    u32 extra = 0;
    T_ERR(ps_frames_alloc(fp, &extra), ENOBUFS);

    struct ps_vnode v = {1};
    ps_frames_set_owner(fp, f[7], &v, ps_page_id_from(42));
    T_CHECK(ps_frames_owner(fp, f[7]) == &v);
    T_EQ(ps_frames_owner_id(fp, f[7]).raw, 42);
    ps_frames_set_evictable(fp, f[7], true);
    ps_frames_touch(fp, f[7]);
    T_EQ(ps_frames_evictable_count(fp), 1);
    ps_frames_release(fp, f[7]);
    T_EQ(ps_frames_free_count(fp), 1);
    T_EQ(ps_frames_evictable_count(fp), 0);
    T_CHECK(ps_frames_owner(fp, f[7]) == NULL);
    T_EQ(ps_frames_owner_id(fp, f[7]).raw, PS_FRAMES_NO_OWNER.raw);

    T_OK(ps_frames_alloc(fp, &extra));
    T_EQ(extra, f[7]);
    T_CHECK(ps_frames_owner(fp, extra) == NULL);
    T_EQ(ps_frames_free_count(fp), 0);

    ps_frames_set_owner(fp, f[3], &v, ps_page_id_from(7));
    ps_frames_set_evictable(fp, f[3], true);
    ps_frames_touch(fp, f[3]);
    ps_frames_detach(fp, f[3]);
    T_CHECK(ps_frames_owner(fp, f[3]) == NULL);
    T_EQ(ps_frames_owner_id(fp, f[3]).raw, PS_FRAMES_NO_OWNER.raw);
    T_EQ(ps_frames_evictable_count(fp), 0);
    T_EQ(ps_frames_free_count(fp), 0);

    /* A stale reference bit would give the reused frame a second chance. */
    ps_frames_set_evictable(fp, extra, true);
    u32 c = 99;
    T_OK(ps_frames_clock(fp, &c));
    T_EQ(c, extra);

    for (u32 i = 0; i < 16; i++) {
        ps_frames_release(fp, f[i]);
    }
    T_EQ(ps_frames_free_count(fp), 16);
    T_EQ(ps_frames_evictable_count(fp), 0);
    ps_frames_destroy(fp);
}

static void test_clock_order(void)
{
    ps_frames *fp = NULL;
    u32 f[16];
    u32 c = 0;

    T_OK(ps_frames_create(4096, 16 * 4096, NULL, &fp));
    for (u32 i = 0; i < 16; i++) {
        T_OK(ps_frames_alloc(fp, &f[i]));
    }
    T_ERR(ps_frames_clock(fp, &c), ENOENT);

    ps_frames_set_evictable(fp, 3, true);
    ps_frames_set_evictable(fp, 5, true);
    T_EQ(ps_frames_evictable_count(fp), 2);
    ps_frames_set_evictable(fp, 5, true);
    T_EQ(ps_frames_evictable_count(fp), 2);

    ps_frames_touch(fp, 5);
    T_OK(ps_frames_clock(fp, &c));
    T_EQ(c, 3);
    T_OK(ps_frames_clock(fp, &c)); /* frame 5 has used its second chance */
    T_EQ(c, 5);
    T_OK(ps_frames_clock(fp, &c));
    T_EQ(c, 3);

    ps_frames_set_evictable(fp, 3, false);
    T_EQ(ps_frames_evictable_count(fp), 1);
    T_OK(ps_frames_clock(fp, &c));
    T_EQ(c, 5);
    ps_frames_set_evictable(fp, 5, false);
    T_EQ(ps_frames_evictable_count(fp), 0);
    ps_frames_set_evictable(fp, 5, false);
    T_EQ(ps_frames_evictable_count(fp), 0);
    T_ERR(ps_frames_clock(fp, &c), ENOENT);

    /* With every frame referenced, a single sweep would find no candidate. */
    for (u32 i = 0; i < 16; i++) {
        ps_frames_set_evictable(fp, i, true);
        ps_frames_touch(fp, i);
    }
    T_EQ(ps_frames_evictable_count(fp), 16);
    T_OK(ps_frames_clock(fp, &c));
    T_CHECK(c < 16);

    for (u32 i = 0; i < 16; i++) {
        ps_frames_set_evictable(fp, i, false);
    }
    ps_frames_touch(fp, 9);
    T_ERR(ps_frames_clock(fp, &c), ENOENT);

    for (u32 i = 0; i < 16; i++) {
        ps_frames_release(fp, f[i]);
    }
    ps_frames_destroy(fp);
}

enum {
    CONC_THREADS = 4,
    CONC_ITERS = 20000
};

typedef struct conc_arg {
    ps_frames *fp;
    struct ps_vnode tag;
} conc_arg;

static void *conc_worker(void *p)
{
    conc_arg *a = p;
    u32 held[4];
    u32 n = 0;

    for (u32 i = 0; i < CONC_ITERS; i++) {
        u32 f = 0;
        if (n < 4 && ps_frames_alloc(a->fp, &f) == 0) {
            ps_frames_set_owner(a->fp, f, &a->tag, ps_page_id_from(1));
            *(u32 *) ps_frame(a->fp, f) = i;
            held[n++] = f;
        }
        u32 c = 0;
        if (ps_frames_clock(a->fp, &c) == 0) {
            T_CHECK(c < ps_frames_count(a->fp));
        }
        if (n > 0) {
            u32 f2 = held[i % n];
            ps_frames_touch(a->fp, f2);
            T_CHECK(ps_frames_owner(a->fp, f2) == &a->tag);
            ps_frames_set_evictable(a->fp, f2, (i & 1) != 0);
        }
        if (n == 4 || (n > 0 && i % 3 == 0)) {
            ps_frames_release(a->fp, held[--n]);
        }
    }
    while (n > 0) {
        ps_frames_release(a->fp, held[--n]);
    }
    return NULL;
}

static void test_concurrent(void)
{
    ps_frames *fp = NULL;
    pthread_t th[CONC_THREADS];
    conc_arg args[CONC_THREADS];

    T_OK(ps_frames_create(4096, 16 * 4096, NULL, &fp));
    for (int i = 0; i < CONC_THREADS; i++) {
        args[i].fp = fp;
        args[i].tag.tag = i;
        T_EQ(pthread_create(&th[i], NULL, conc_worker, &args[i]), 0);
    }
    for (int i = 0; i < CONC_THREADS; i++) {
        T_EQ(pthread_join(th[i], NULL), 0);
    }
    T_EQ(ps_frames_free_count(fp), 16);
    T_EQ(ps_frames_evictable_count(fp), 0);
    ps_frames_destroy(fp);
}

int main(int argc, char **argv)
{
    t_init(argc, argv);
    T_RUN(test_create_limits);
    T_RUN(test_alloc_release);
    T_RUN(test_clock_order);
    T_RUN(test_concurrent);
    return t_done();
}
