#include "ptable.h"
#include "test.h"

#include "pagestore/types.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>

static void test_create_sizes(void)
{
    ps_ptable *pt = NULL;

    T_OK(ps_ptable_create(0, &pt));
    T_EQ(ps_ptable_nbuckets(pt), 64);
    T_EQ(ps_ptable_nlatches(pt), 64);
    ps_ptable_destroy(pt);

    T_OK(ps_ptable_create(16, &pt));
    T_EQ(ps_ptable_nbuckets(pt), 64);
    ps_ptable_destroy(pt);

    T_OK(ps_ptable_create(33, &pt));
    T_EQ(ps_ptable_nbuckets(pt), 128);
    T_EQ(ps_ptable_nlatches(pt), 128);
    ps_ptable_destroy(pt);

    T_OK(ps_ptable_create(1000, &pt));
    T_EQ(ps_ptable_nbuckets(pt), 2048);
    T_EQ(ps_ptable_nlatches(pt), 1024);
    T_CHECK(ps_ptable_bytes(pt) >= 2048 * sizeof(ps_entry *));
    ps_ptable_destroy(pt);

    pt = NULL;
    T_ERR(ps_ptable_create(UINT32_MAX, &pt), EINVAL);
    T_CHECK(pt == NULL);

    ps_ptable_destroy(NULL);
}

static void test_get_find_release(void)
{
    ps_ptable *pt = NULL;
    T_OK(ps_ptable_create(16, &pt));
    const u64 base = ps_ptable_bytes(pt);

    ps_latch(pt, ps_page_id_from(42));
    T_CHECK(ps_entry_find(pt, ps_page_id_from(42)) == NULL);
    ps_entry *e = NULL;
    T_OK(ps_entry_get(pt, ps_page_id_from(42), &e));
    T_CHECK(e != NULL);
    T_EQ(e->id.raw, 42);
    T_EQ(e->flags, 0);
    T_EQ(e->waiters, 0);
    T_EQ(atomic_load(&e->dirty), 0);
    T_CHECK(e->head == NULL);
    T_CHECK(ps_entry_find(pt, ps_page_id_from(42)) == e);

    ps_entry *again = NULL;
    T_OK(ps_entry_get(pt, ps_page_id_from(42), &again));
    T_CHECK(again == e);
    const u64 with_entry = ps_ptable_bytes(pt);
    T_CHECK(with_entry > base);

    ps_entry_maybe_release(pt, e);
    T_CHECK(ps_entry_find(pt, ps_page_id_from(42)) == NULL);
    ps_unlatch(pt, ps_page_id_from(42));
    T_CHECK(ps_ptable_bytes(pt) < with_entry);
    T_EQ(ps_ptable_bytes(pt), base);

    ps_ptable_destroy(pt);
}

static void test_release_blocked(void)
{
    ps_ptable *pt = NULL;
    T_OK(ps_ptable_create(16, &pt));
    const ps_page_id id = ps_page_id_from(9);
    ps_entry *e = NULL;

    ps_latch(pt, id);
    T_OK(ps_entry_get(pt, id, &e));

    atomic_store(&e->dirty, 2);
    ps_entry_maybe_release(pt, e);
    T_CHECK(ps_entry_find(pt, id) == e);
    atomic_store(&e->dirty, 0);

    e->flags = PS_E_LOADING;
    ps_entry_maybe_release(pt, e);
    T_CHECK(ps_entry_find(pt, id) == e);
    e->flags = 0;

    e->waiters = 1;
    ps_entry_maybe_release(pt, e);
    T_CHECK(ps_entry_find(pt, id) == e);
    e->waiters = 0;

    ps_vnode *v = NULL;
    T_OK(ps_vnode_new(pt, &v));
    v->entry = e;
    e->head = v;
    ps_entry_maybe_release(pt, e);
    T_CHECK(ps_entry_find(pt, id) == e);
    e->head = NULL;
    ps_vnode_delete(pt, v);

    ps_entry_maybe_release(pt, e);
    T_CHECK(ps_entry_find(pt, id) == NULL);
    ps_unlatch(pt, id);

    ps_ptable_destroy(pt);
}

static void test_vnode_alloc(void)
{
    ps_ptable *pt = NULL;
    T_OK(ps_ptable_create(16, &pt));
    const u64 base = ps_ptable_bytes(pt);

    ps_vnode *v = NULL;
    T_OK(ps_vnode_new(pt, &v));
    T_EQ(v->lsn.raw, 0);
    T_CHECK(v->older == NULL);
    T_CHECK(v->entry == NULL);
    T_EQ(v->frame, PS_NO_FRAME);
    T_EQ(v->pins, 0);
    T_EQ(v->kind, PS_V_DATA);
    T_EQ(v->flags, 0);
    T_CHECK(v->retire_next == NULL);
    T_EQ(v->retire_r.raw, 0);
    const u64 one_slab = ps_ptable_bytes(pt) - base;
    T_CHECK(one_slab >= 256 * sizeof(ps_vnode));

    v->lsn = ps_lsn_from(77);
    v->frame = 3;
    v->pins = 2;
    v->kind = PS_V_FREE;
    v->flags = PS_VF_ON_DISK | PS_VF_RETIRED;
    v->retire_r = ps_lsn_from(99);
    ps_vnode_delete(pt, v);
    ps_vnode *w = NULL;
    T_OK(ps_vnode_new(pt, &w));
    T_CHECK(w == v);
    T_EQ(w->lsn.raw, 0);
    T_EQ(w->frame, PS_NO_FRAME);
    T_EQ(w->pins, 0);
    T_EQ(w->kind, PS_V_DATA);
    T_EQ(w->flags, 0);
    T_EQ(w->retire_r.raw, 0);
    ps_vnode_delete(pt, w);

    enum {
        N = 600
    };
    ps_vnode *nodes[N];
    for (u32 i = 0; i < N; i++) {
        T_OK(ps_vnode_new(pt, &nodes[i]));
        nodes[i]->lsn = ps_lsn_from(i + 1);
    }
    for (u32 i = 0; i < N; i++) {
        T_EQ(nodes[i]->lsn.raw, i + 1);
    }
    T_EQ(ps_ptable_bytes(pt) - base, 3 * one_slab);
    for (u32 i = 0; i < N; i++) {
        ps_vnode_delete(pt, nodes[i]);
    }
    for (u32 i = 0; i < N; i++) {
        T_OK(ps_vnode_new(pt, &nodes[i]));
    }
    T_EQ(ps_ptable_bytes(pt) - base, 3 * one_slab);
    /* Leave nodes live here so the leak checker covers slab teardown. */
    ps_ptable_destroy(pt);
}

static void test_chain_visible(void)
{
    ps_ptable *pt = NULL;
    T_OK(ps_ptable_create(16, &pt));
    const ps_page_id id = ps_page_id_from(5);
    ps_entry *e = NULL;

    ps_latch(pt, id);
    T_OK(ps_entry_get(pt, id, &e));
    T_CHECK(ps_chain_visible(e, ps_lsn_from(1000)) == NULL);

    const u64 lsns[] = {40, 87, 120, 130};
    ps_vnode *v[4];
    for (u32 i = 0; i < 4; i++) {
        T_OK(ps_vnode_new(pt, &v[i]));
        v[i]->lsn = ps_lsn_from(lsns[i]);
        v[i]->entry = e;
        v[i]->older = e->head;
        e->head = v[i];
    }
    T_CHECK(ps_chain_visible(e, ps_lsn_from(100)) == v[1]);
    T_CHECK(ps_chain_visible(e, ps_lsn_from(87)) == v[1]);
    T_CHECK(ps_chain_visible(e, ps_lsn_from(86)) == v[0]);
    T_CHECK(ps_chain_visible(e, ps_lsn_from(40)) == v[0]);
    T_CHECK(ps_chain_visible(e, ps_lsn_from(39)) == NULL);
    T_CHECK(ps_chain_visible(e, PS_LSN_NONE) == NULL);
    T_CHECK(ps_chain_visible(e, ps_lsn_from(125)) == v[2]);
    T_CHECK(ps_chain_visible(e, ps_lsn_from(1000)) == v[3]);

    /* PS_LSN_NONE represents a disk version whose LSN has yet to be loaded. */
    ps_vnode *disk = NULL;
    T_OK(ps_vnode_new(pt, &disk));
    disk->flags = PS_VF_ON_DISK;
    disk->entry = e;
    v[0]->older = disk;
    T_CHECK(ps_chain_visible(e, ps_lsn_from(39)) == disk);
    T_CHECK(ps_chain_visible(e, PS_LSN_NONE) == disk);
    ps_unlatch(pt, id);

    ps_ptable_destroy(pt);
}

static void test_colliding_ids(void)
{
    ps_ptable *pt = NULL;
    T_OK(ps_ptable_create(16, &pt));
    const u32 nb = ps_ptable_nbuckets(pt);
    const u64 base = ps_ptable_bytes(pt);
    enum {
        K = 100
    };
    ps_entry *es[K];

    /* Force collisions by using multiples of the bucket count. */
    for (u32 k = 0; k < K; k++) {
        const ps_page_id id = ps_page_id_from(k * nb);
        ps_latch(pt, id);
        T_OK(ps_entry_get(pt, id, &es[k]));
        T_EQ(es[k]->id.raw, id.raw);
        ps_unlatch(pt, id);
    }
    for (u32 k = 0; k < K; k++) {
        const ps_page_id id = ps_page_id_from(k * nb);
        ps_latch(pt, id);
        T_CHECK(ps_entry_find(pt, id) == es[k]);
        ps_unlatch(pt, id);
    }
    ps_latch(pt, ps_page_id_from(1));
    T_CHECK(ps_entry_find(pt, ps_page_id_from(1)) == NULL);
    ps_unlatch(pt, ps_page_id_from(1));

    /* Remove middle and end entries to exercise both unlink paths. */
    const u32 order[] = {50, 0, K - 1, 1, 98};
    for (u32 i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        const u32 k = order[i];
        const ps_page_id id = ps_page_id_from(k * nb);
        ps_latch(pt, id);
        ps_entry_maybe_release(pt, es[k]);
        T_CHECK(ps_entry_find(pt, id) == NULL);
        es[k] = NULL;
        ps_unlatch(pt, id);
    }
    for (u32 k = 0; k < K; k++) {
        const ps_page_id id = ps_page_id_from(k * nb);
        ps_latch(pt, id);
        T_CHECK(ps_entry_find(pt, id) == es[k]);
        if (es[k] != NULL) {
            ps_entry_maybe_release(pt, es[k]);
            T_CHECK(ps_entry_find(pt, id) == NULL);
        }
        ps_unlatch(pt, id);
    }
    T_EQ(ps_ptable_bytes(pt), base);

    ps_entry *top = NULL;
    ps_latch(pt, ps_page_id_from(UINT32_MAX - 1));
    T_OK(ps_entry_get(pt, ps_page_id_from(UINT32_MAX - 1), &top));
    T_CHECK(ps_entry_find(pt, ps_page_id_from(UINT32_MAX - 1)) == top);
    ps_unlatch(pt, ps_page_id_from(UINT32_MAX - 1));

    ps_ptable_destroy(pt);
}

typedef struct waiter_args {
    ps_ptable *pt;
    bool flag; /* latch(7) */
    bool woke; /* latch(7) */
    atomic_bool waiting;
} waiter_args;

static void *waiter(void *arg)
{
    waiter_args *a = arg;
    ps_latch(a->pt, ps_page_id_from(7));
    atomic_store(&a->waiting, true);
    while (!a->flag) {
        ps_latch_wait(a->pt, ps_page_id_from(7));
    }
    a->woke = true;
    ps_unlatch(a->pt, ps_page_id_from(7));
    return NULL;
}

static void test_latch_wait_broadcast(void)
{
    waiter_args a = {.flag = false, .woke = false};
    atomic_init(&a.waiting, false);
    T_OK(ps_ptable_create(16, &a.pt));

    pthread_t th;
    T_OK(pthread_create(&th, NULL, waiter, &a));
    while (!atomic_load(&a.waiting)) {
        sched_yield();
    }
    /* Acquiring the latch means the waiter has entered ps_latch_wait. */
    ps_latch(a.pt, ps_page_id_from(7));
    T_CHECK(!a.woke);
    a.flag = true;
    ps_latch_broadcast(a.pt, ps_page_id_from(7));
    ps_unlatch(a.pt, ps_page_id_from(7));
    T_OK(pthread_join(th, NULL));
    T_CHECK(a.woke);

    ps_ptable_destroy(a.pt);
}

enum {
    CONC_THREADS = 4,
    CONC_ITERS = 20000
};

typedef struct conc_args {
    ps_ptable *pt;
    u32 tid;
} conc_args;

static void *conc_worker(void *arg)
{
    conc_args *a = arg;
    for (u32 i = 0; i < CONC_ITERS; i++) {
        const ps_page_id id = ps_page_id_from((i * 7 + a->tid) % 300);
        ps_latch(a->pt, id);
        ps_entry *e = NULL;
        T_OK(ps_entry_get(a->pt, id, &e));
        T_EQ(e->id.raw, id.raw);
        if (e->head == NULL) {
            ps_vnode *v = NULL;
            T_OK(ps_vnode_new(a->pt, &v));
            v->entry = e;
            v->lsn = ps_lsn_from(i);
            e->head = v;
        } else {
            ps_vnode *v = e->head;
            T_CHECK(v->entry == e);
            e->head = NULL;
            ps_vnode_delete(a->pt, v);
            ps_entry_maybe_release(a->pt, e);
        }
        ps_unlatch(a->pt, id);
        const u64 bytes = ps_ptable_bytes(a->pt);
        PS_UNUSED(bytes);
    }
    return NULL;
}

static void test_concurrent(void)
{
    ps_ptable *pt = NULL;
    T_OK(ps_ptable_create(64, &pt));
    pthread_t th[CONC_THREADS];
    conc_args args[CONC_THREADS];
    for (u32 i = 0; i < CONC_THREADS; i++) {
        args[i] = (conc_args) {.pt = pt, .tid = i};
        T_OK(pthread_create(&th[i], NULL, conc_worker, &args[i]));
    }
    for (u32 i = 0; i < CONC_THREADS; i++) {
        T_OK(pthread_join(th[i], NULL));
    }
    ps_ptable_destroy(pt);
}

int main(int argc, char **argv)
{
    t_init(argc, argv);
    T_RUN(test_create_sizes);
    T_RUN(test_get_find_release);
    T_RUN(test_release_blocked);
    T_RUN(test_vnode_alloc);
    T_RUN(test_chain_visible);
    T_RUN(test_colliding_ids);
    T_RUN(test_latch_wait_broadcast);
    T_RUN(test_concurrent);
    return t_done();
}
