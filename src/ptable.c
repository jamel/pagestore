/*
 * Page entries live in fixed hash buckets. Each bucket maps to one page
 * latch. Version nodes come from reusable slabs with their own mutex.
 * Returned nodes go on the free list. Slabs are freed with the table.
 */

#include "ptable.h"
#include "util.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define PS_PTABLE_MIN_BUCKETS 64u
#define PS_PTABLE_MAX_LATCHES 1024u
#define PS_PTABLE_MAX_BUCKETS (1ull << 31)
#define PS_VNODE_SLAB 256u

typedef struct ps_latch_slot {
    pthread_mutex_t mu;
    pthread_cond_t cv;
} ps_latch_slot;

typedef struct ps_vnode_slab {
    struct ps_vnode_slab *next;
    ps_vnode nodes[PS_VNODE_SLAB];
} ps_vnode_slab;

struct ps_ptable {
    u32 nbuckets;
    u32 nlatches;
    ps_entry **buckets;
    ps_latch_slot *latches;
    u32 nlatches_init; /* used to clean up a partial initialization */

    _Atomic u64 nentries;
    _Atomic u64 nslabs;

    /* Slab list and free nodes. */
    pthread_mutex_t node_mu;
    bool node_mu_init;
    ps_vnode_slab *slabs;
    ps_vnode *free_nodes; /* linked through older */
};

static inline ps_latch_slot *latch_of(ps_ptable *pt, ps_page_id id)
{
    return &pt->latches[id.raw & (pt->nlatches - 1)];
}

static inline ps_entry **bucket_of(ps_ptable *pt, ps_page_id id)
{
    return &pt->buckets[id.raw & (pt->nbuckets - 1)];
}

int ps_ptable_create(u32 nframes, ps_ptable **out)
{
    u64 nb = PS_PTABLE_MIN_BUCKETS;
    while (nb < 2 * (u64) nframes) {
        nb <<= 1;
    }
    if (nb > PS_PTABLE_MAX_BUCKETS) {
        return EINVAL;
    }

    ps_ptable *pt = calloc(1, sizeof(*pt));
    if (pt == NULL) {
        return ENOMEM;
    }
    pt->nbuckets = (u32) nb;
    pt->nlatches = PS_MIN(pt->nbuckets, PS_PTABLE_MAX_LATCHES);
    atomic_init(&pt->nentries, 0);
    atomic_init(&pt->nslabs, 0);
    pt->buckets = calloc(pt->nbuckets, sizeof(*pt->buckets));
    pt->latches = calloc(pt->nlatches, sizeof(*pt->latches));
    if (pt->buckets == NULL || pt->latches == NULL) {
        ps_ptable_destroy(pt);
        return ENOMEM;
    }

    int rv = pthread_mutex_init(&pt->node_mu, NULL);
    if (rv != 0) {
        ps_ptable_destroy(pt);
        return rv;
    }
    pt->node_mu_init = true;

    for (u32 i = 0; i < pt->nlatches; i++) {
        ps_latch_slot *l = &pt->latches[i];
        rv = pthread_mutex_init(&l->mu, NULL);
        if (rv != 0) {
            ps_ptable_destroy(pt);
            return rv;
        }
        rv = pthread_cond_init(&l->cv, NULL);
        if (rv != 0) {
            pthread_mutex_destroy(&l->mu);
            ps_ptable_destroy(pt);
            return rv;
        }
        pt->nlatches_init++;
    }

    *out = pt;
    return 0;
}

void ps_ptable_destroy(ps_ptable *pt)
{
    if (pt == NULL) {
        return;
    }
    if (pt->buckets != NULL) {
        for (u32 b = 0; b < pt->nbuckets; b++) {
            ps_entry *e = pt->buckets[b];
            while (e != NULL) {
                ps_entry *next = e->hash_next;
                free(e);
                e = next;
            }
        }
        free(pt->buckets);
    }
    ps_vnode_slab *s = pt->slabs;
    while (s != NULL) {
        ps_vnode_slab *next = s->next;
        free(s);
        s = next;
    }
    for (u32 i = 0; i < pt->nlatches_init; i++) {
        pthread_cond_destroy(&pt->latches[i].cv);
        pthread_mutex_destroy(&pt->latches[i].mu);
    }
    free(pt->latches);
    if (pt->node_mu_init) {
        pthread_mutex_destroy(&pt->node_mu);
    }
    free(pt);
}

u32 ps_ptable_nbuckets(const ps_ptable *pt)
{
    return pt->nbuckets;
}

u32 ps_ptable_nlatches(const ps_ptable *pt)
{
    return pt->nlatches;
}

void ps_latch(ps_ptable *pt, ps_page_id id)
{
    const int rv = pthread_mutex_lock(&latch_of(pt, id)->mu);
    PS_ASSERT(rv == 0);
    PS_UNUSED(rv);
}

void ps_unlatch(ps_ptable *pt, ps_page_id id)
{
    const int rv = pthread_mutex_unlock(&latch_of(pt, id)->mu);
    PS_ASSERT(rv == 0);
    PS_UNUSED(rv);
}

void ps_latch_wait(ps_ptable *pt, ps_page_id id)
{
    ps_latch_slot *l = latch_of(pt, id);
    const int rv = pthread_cond_wait(&l->cv, &l->mu);
    PS_ASSERT(rv == 0);
    PS_UNUSED(rv);
}

void ps_latch_broadcast(ps_ptable *pt, ps_page_id id)
{
    const int rv = pthread_cond_broadcast(&latch_of(pt, id)->cv);
    PS_ASSERT(rv == 0);
    PS_UNUSED(rv);
}

ps_entry *ps_entry_find(ps_ptable *pt, ps_page_id id)
{
    for (ps_entry *e = *bucket_of(pt, id); e != NULL; e = e->hash_next) {
        if (ps_page_id_eq(e->id, id)) {
            return e;
        }
    }
    return NULL;
}

int ps_entry_get(ps_ptable *pt, ps_page_id id, ps_entry **out)
{
    ps_entry *e = ps_entry_find(pt, id);
    if (e == NULL) {
        e = calloc(1, sizeof(*e));
        if (e == NULL) {
            return ENOMEM;
        }
        e->id = id;
        atomic_init(&e->dirty, 0);
        ps_entry **b = bucket_of(pt, id);
        e->hash_next = *b;
        *b = e;
        atomic_fetch_add_explicit(&pt->nentries, 1, memory_order_relaxed);
    }
    *out = e;
    return 0;
}

void ps_entry_maybe_release(ps_ptable *pt, ps_entry *e)
{
    if (e->head != NULL || (e->flags & PS_E_LOADING) != 0 || e->waiters != 0 ||
        atomic_load(&e->dirty) != 0) {
        return;
    }
    ps_entry **p = bucket_of(pt, e->id);
    while (*p != e) {
        PS_ASSERT(*p != NULL);
        p = &(*p)->hash_next;
    }
    *p = e->hash_next;
    free(e);
    atomic_fetch_sub_explicit(&pt->nentries, 1, memory_order_relaxed);
}

int ps_vnode_new(ps_ptable *pt, ps_vnode **out)
{
    pthread_mutex_lock(&pt->node_mu);
    if (pt->free_nodes == NULL) {
        ps_vnode_slab *s = malloc(sizeof(*s));
        if (s == NULL) {
            pthread_mutex_unlock(&pt->node_mu);
            return ENOMEM;
        }
        s->next = pt->slabs;
        pt->slabs = s;
        /* The free list hands out this slab in address order. */
        for (u32 i = PS_VNODE_SLAB; i-- > 0;) {
            s->nodes[i].older = pt->free_nodes;
            pt->free_nodes = &s->nodes[i];
        }
        atomic_fetch_add_explicit(&pt->nslabs, 1, memory_order_relaxed);
    }
    ps_vnode *v = pt->free_nodes;
    pt->free_nodes = v->older;
    pthread_mutex_unlock(&pt->node_mu);

    memset(v, 0, sizeof(*v));
    v->kind = PS_V_DATA;
    v->frame = PS_NO_FRAME;
    *out = v;
    return 0;
}

void ps_vnode_delete(ps_ptable *pt, ps_vnode *v)
{
    if (v == NULL) {
        return;
    }
    pthread_mutex_lock(&pt->node_mu);
    v->older = pt->free_nodes;
    pt->free_nodes = v;
    pthread_mutex_unlock(&pt->node_mu);
}

ps_vnode *ps_chain_visible(const ps_entry *e, ps_lsn lsn)
{
    for (ps_vnode *v = e->head; v != NULL; v = v->older) {
        if (ps_lsn_le(v->lsn, lsn)) {
            return v;
        }
    }
    return NULL;
}

u64 ps_ptable_bytes(const ps_ptable *pt)
{
    return (u64) pt->nbuckets * sizeof(ps_entry *) +
        atomic_load_explicit(&pt->nentries, memory_order_relaxed) * sizeof(ps_entry) +
        atomic_load_explicit(&pt->nslabs, memory_order_relaxed) * sizeof(ps_vnode_slab);
}
