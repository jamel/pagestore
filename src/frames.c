/*
 * A mutex guards the free stack. CLOCK reads atomic flags without taking
 * page latches.
 */

#include "frames.h"

#include "format.h"
#include "util.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

#define PS_FRAME_ALIGN 4096u
#define PS_FRAMES_MIN 16u

struct ps_frames {
    u8 *mem;
    bool own_mem;
    u32 page_size;
    u32 count;

    _Atomic(struct ps_vnode *) *owner;
    _Atomic u32 *owner_id;
    atomic_bool *evictable;
    atomic_bool *ref;

    _Atomic u32 nevictable;
    _Atomic u32 nfree;
    _Atomic u64 hand; /* the next clockwise index is hand % count */

    /* mu guards the free stack and the double-release check. */
    pthread_mutex_t mu;
    u32 *stack;
    u32 top;
    bool *is_free;
};

static void frames_free(ps_frames *fp)
{
    if (fp->own_mem) {
        free(fp->mem);
    }
    free(fp->owner);
    free(fp->owner_id);
    free(fp->evictable);
    free(fp->ref);
    free(fp->stack);
    free(fp->is_free);
    free(fp);
}

int ps_frames_create(u32 page_size, usize cache_size, void *memory, ps_frames **out)
{
    if (!ps_page_size_valid(page_size)) {
        return EINVAL;
    }
    const usize n = cache_size / page_size;
    if (n < PS_FRAMES_MIN || n >= UINT32_MAX) {
        return EINVAL;
    }
    if (memory != NULL && (uintptr_t) memory % PS_FRAME_ALIGN != 0) {
        return EINVAL;
    }

    ps_frames *fp = calloc(1, sizeof(*fp));
    if (fp == NULL) {
        return ENOMEM;
    }
    fp->page_size = page_size;
    fp->count = (u32) n;
    fp->owner = calloc(n, sizeof(*fp->owner));
    fp->owner_id = calloc(n, sizeof(*fp->owner_id));
    fp->evictable = calloc(n, sizeof(*fp->evictable));
    fp->ref = calloc(n, sizeof(*fp->ref));
    fp->stack = calloc(n, sizeof(*fp->stack));
    fp->is_free = calloc(n, sizeof(*fp->is_free));
    if (fp->owner == NULL || fp->owner_id == NULL || fp->evictable == NULL || fp->ref == NULL ||
        fp->stack == NULL || fp->is_free == NULL) {
        frames_free(fp);
        return ENOMEM;
    }

    if (memory != NULL) {
        fp->mem = memory;
    } else {
        void *p = NULL;
        const int rv = posix_memalign(&p, PS_FRAME_ALIGN, n * page_size);
        if (rv != 0) {
            frames_free(fp);
            return rv == EINVAL ? EINVAL : ENOMEM;
        }
        fp->mem = p;
        fp->own_mem = true;
    }

    const int rv = pthread_mutex_init(&fp->mu, NULL);
    if (rv != 0) {
        frames_free(fp);
        return rv;
    }

    for (u32 i = 0; i < fp->count; i++) {
        atomic_init(&fp->owner[i], NULL);
        atomic_init(&fp->owner_id[i], PS_FRAMES_NO_OWNER);
        atomic_init(&fp->evictable[i], false);
        atomic_init(&fp->ref[i], false);
        /* Start with frame 0 at the top. Recycled frames come back in LIFO order. */
        fp->stack[i] = fp->count - 1 - i;
        fp->is_free[i] = true;
    }
    fp->top = fp->count;
    atomic_init(&fp->nevictable, 0);
    atomic_init(&fp->nfree, fp->count);
    atomic_init(&fp->hand, 0);

    *out = fp;
    return 0;
}

void ps_frames_destroy(ps_frames *fp)
{
    if (fp == NULL) {
        return;
    }
    pthread_mutex_destroy(&fp->mu);
    frames_free(fp);
}

u32 ps_frames_count(const ps_frames *fp)
{
    return fp->count;
}

void *ps_frame(const ps_frames *fp, u32 f)
{
    PS_ASSERT(f < fp->count);
    return fp->mem + (usize) f * fp->page_size;
}

int ps_frames_alloc(ps_frames *fp, u32 *f)
{
    pthread_mutex_lock(&fp->mu);
    if (fp->top == 0) {
        pthread_mutex_unlock(&fp->mu);
        return ENOBUFS;
    }
    const u32 i = fp->stack[--fp->top];
    PS_ASSERT(fp->is_free[i]);
    fp->is_free[i] = false;
    atomic_fetch_sub_explicit(&fp->nfree, 1, memory_order_relaxed);
    pthread_mutex_unlock(&fp->mu);

    *f = i;
    return 0;
}

void ps_frames_detach(ps_frames *fp, u32 f)
{
    PS_ASSERT(f < fp->count);
    ps_frames_set_evictable(fp, f, false);
    atomic_store_explicit(&fp->owner_id[f], PS_FRAMES_NO_OWNER, memory_order_release);
    atomic_store_explicit(&fp->owner[f], NULL, memory_order_release);
    atomic_store_explicit(&fp->ref[f], false, memory_order_relaxed);
}

void ps_frames_release(ps_frames *fp, u32 f)
{
    ps_frames_detach(fp, f);

    pthread_mutex_lock(&fp->mu);
    PS_ASSERT(!fp->is_free[f]);
    PS_ASSERT(fp->top < fp->count);
    fp->is_free[f] = true;
    fp->stack[fp->top++] = f;
    atomic_fetch_add_explicit(&fp->nfree, 1, memory_order_relaxed);
    pthread_mutex_unlock(&fp->mu);
}

u32 ps_frames_free_count(const ps_frames *fp)
{
    return atomic_load_explicit(&fp->nfree, memory_order_relaxed);
}

void ps_frames_set_owner(ps_frames *fp, u32 f, struct ps_vnode *owner, u32 id)
{
    PS_ASSERT(f < fp->count);
    atomic_store_explicit(&fp->owner[f], owner, memory_order_release);
    atomic_store_explicit(&fp->owner_id[f], id, memory_order_release);
}

struct ps_vnode *ps_frames_owner(const ps_frames *fp, u32 f)
{
    PS_ASSERT(f < fp->count);
    return atomic_load_explicit(&fp->owner[f], memory_order_acquire);
}

u32 ps_frames_owner_id(const ps_frames *fp, u32 f)
{
    PS_ASSERT(f < fp->count);
    return atomic_load_explicit(&fp->owner_id[f], memory_order_acquire);
}

void ps_frames_set_evictable(ps_frames *fp, u32 f, bool on)
{
    PS_ASSERT(f < fp->count);
    const bool was = atomic_exchange_explicit(&fp->evictable[f], on, memory_order_acq_rel);
    if (was == on) {
        return;
    }
    if (on) {
        atomic_fetch_add_explicit(&fp->nevictable, 1, memory_order_relaxed);
    } else {
        atomic_fetch_sub_explicit(&fp->nevictable, 1, memory_order_relaxed);
    }
}

u32 ps_frames_evictable_count(const ps_frames *fp)
{
    return atomic_load_explicit(&fp->nevictable, memory_order_relaxed);
}

void ps_frames_touch(ps_frames *fp, u32 f)
{
    PS_ASSERT(f < fp->count);
    /* Avoid taking the cache line exclusive on repeated accesses. */
    if (!atomic_load_explicit(&fp->ref[f], memory_order_relaxed)) {
        atomic_store_explicit(&fp->ref[f], true, memory_order_relaxed);
    }
}

int ps_frames_clock(ps_frames *fp, u32 *f)
{
    const u64 steps = 2 * (u64) fp->count;

    for (u64 s = 0; s < steps; s++) {
        const u64 h = atomic_fetch_add_explicit(&fp->hand, 1, memory_order_relaxed);
        const u32 i = (u32) (h % fp->count);
        if (!atomic_load_explicit(&fp->evictable[i], memory_order_acquire)) {
            continue;
        }
        /* Clear a set bit to give the frame a second chance. */
        if (atomic_load_explicit(&fp->ref[i], memory_order_relaxed)) {
            atomic_store_explicit(&fp->ref[i], false, memory_order_relaxed);
            continue;
        }
        bool clear = false;
        if (!atomic_compare_exchange_strong_explicit(
                &fp->ref[i],
                &clear,
                true,
                memory_order_relaxed,
                memory_order_relaxed)) {
            continue;
        }
        *f = i;
        return 0;
    }
    return ENOENT;
}
