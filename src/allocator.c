/*
 * A free count for each group lets the search skip full groups without
 * fetching their bitmaps. Pending changes stay in a per-group delta until
 * the caller applies them to the bitmaps at commit.
 */

#include "allocator.h"

#include "bitmap.h"
#include "format.h"
#include "util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define PS_ALLOC_PAGE_HEADER 24u
/* The largest valid page ID is UINT32_MAX - 1. */
#define PS_ALLOC_ID_LIMIT ((u64) UINT32_MAX)

/* A delta bit flips the committed bit. Cancelling an allocation clears its delta bit. */
typedef struct op_group {
    u32 g;
    bool is_new; /* created by this operation, so no committed bitmap */
    bool changed; /* in the operation's changed list */
    const u8 *bits; /* committed bitmap from get (NULL for a new group) */
    u8 *delta; /* allocated on demand and reused across operations */
    i32 net; /* allocations - frees in this group */
} op_group;

struct ps_allocator {
    u32 gsize;
    u32 max_groups;
    u32 group_limit;

    /* Committed state. */
    u32 num_groups;
    u32 cursor; /* <= the lowest group with free_count > 0 */
    u64 allocated;
    u32 cap; /* capacity of free_count and slot, in groups */
    u32 *free_count; /* committed clear bits per group */
    u32 *slot; /* index into recs plus one, or zero if this operation has not touched it */

    /* Current operation. */
    bool in_op;
    u32 op_num_groups;
    bool has_last;
    ps_page_id last;
    op_group *recs;
    u32 nrecs;
    u32 recs_cap; /* recs and changed both have this capacity */
    u32 *changed; /* ascending group indexes */
    u32 nchanged;
};

/* ---- group layout and setup ---- */

u32 ps_group_size(u32 page_size)
{
    return (page_size - PS_ALLOC_PAGE_HEADER) * 8;
}

u32 ps_max_groups(u32 page_size)
{
    u64 g = ps_group_size(page_size);
    return (u32) ((PS_ALLOC_ID_LIMIT + g - 1) / g);
}

/* Number of ids in group g (G, or fewer for the last group). */
static u32 group_nbits(const ps_allocator *a, u32 g)
{
    u64 start = (u64) g * a->gsize;
    u64 left = PS_ALLOC_ID_LIMIT - start;
    return left < a->gsize ? (u32) left : a->gsize;
}

static usize delta_bytes(const ps_allocator *a)
{
    return a->gsize / 8;
}

int ps_allocator_create(u32 page_size, ps_allocator **out)
{
    if (!ps_page_size_valid(page_size)) {
        return EINVAL;
    }
    ps_allocator *a = calloc(1, sizeof(*a));
    if (a == NULL) {
        return ENOMEM;
    }
    a->gsize = ps_group_size(page_size);
    a->max_groups = ps_max_groups(page_size);
    a->group_limit = a->max_groups;
    PS_ASSERT(a->gsize % 64 == 0);
    /* The last group holds at least one user id for every valid page size. */
    PS_ASSERT(group_nbits(a, a->max_groups - 1) >= 2);
    *out = a;
    return 0;
}

void ps_allocator_destroy(ps_allocator *a)
{
    if (a == NULL) {
        return;
    }
    for (u32 i = 0; i < a->recs_cap; i++) {
        free(a->recs[i].delta);
    }
    free(a->recs);
    free(a->changed);
    free(a->free_count);
    free(a->slot);
    free(a);
}

/* Ensures free_count and slot hold at least n groups. */
static int reserve_groups(ps_allocator *a, u32 n)
{
    if (n <= a->cap) {
        return 0;
    }
    u64 want = a->cap == 0 ? 64 : (u64) a->cap * 2;
    if (want < n) {
        want = n;
    }
    if (want > a->max_groups) {
        want = a->max_groups;
    }
    PS_ASSERT(want >= n);

    u32 *fc = realloc(a->free_count, (usize) want * sizeof(*fc));
    if (fc == NULL) {
        return ENOMEM;
    }
    a->free_count = fc;
    u32 *sl = realloc(a->slot, (usize) want * sizeof(*sl));
    if (sl == NULL) {
        return ENOMEM;
    }
    memset(sl + a->cap, 0, (usize) (want - a->cap) * sizeof(*sl));
    a->slot = sl;
    a->cap = (u32) want;
    return 0;
}

/* Ensures recs and changed have room for one more record. */
static int reserve_recs(ps_allocator *a)
{
    if (a->nrecs < a->recs_cap) {
        return 0;
    }
    u32 want = a->recs_cap == 0 ? 8 : a->recs_cap * 2;
    op_group *r = realloc(a->recs, (usize) want * sizeof(*r));
    if (r == NULL) {
        return ENOMEM;
    }
    memset(r + a->recs_cap, 0, (usize) (want - a->recs_cap) * sizeof(*r));
    a->recs = r;
    u32 *c = realloc(a->changed, (usize) want * sizeof(*c));
    if (c == NULL) {
        return ENOMEM;
    }
    a->changed = c;
    a->recs_cap = want;
    return 0;
}

int ps_allocator_init_group(ps_allocator *a, u32 g, const u8 *bits)
{
    PS_ASSERT(!a->in_op);
    if (g != a->num_groups || g >= a->max_groups || !ps_bit_test(bits, 0)) {
        return EINVAL;
    }
    int rv = reserve_groups(a, g + 1);
    if (rv != 0) {
        return rv;
    }
    u32 nbits = group_nbits(a, g);
    u32 set = ps_bit_count_set(bits, nbits);
    a->free_count[g] = nbits - set;
    a->allocated += set - 1;
    /* Keep the cursor at the first group with free IDs once there is one. */
    if (g == 0 || a->free_count[a->cursor] == 0) {
        a->cursor = g;
    }
    a->num_groups = g + 1;
    return 0;
}

u32 ps_allocator_num_groups(const ps_allocator *a)
{
    return a->num_groups;
}

u64 ps_allocator_allocated(const ps_allocator *a)
{
    return a->allocated;
}

/* ---- operation bookkeeping ---- */

static void op_reset(ps_allocator *a)
{
    for (u32 i = 0; i < a->nrecs; i++) {
        a->slot[a->recs[i].g] = 0;
    }
    a->nrecs = 0;
    a->nchanged = 0;
    a->op_num_groups = a->num_groups;
    a->has_last = false;
    a->last = PS_PAGE_ID_NONE;
}

void ps_allocator_op_begin(ps_allocator *a)
{
    PS_ASSERT(!a->in_op);
    a->in_op = true;
    op_reset(a);
}

static op_group *find_rec(const ps_allocator *a, u32 g)
{
    if (g >= a->cap || a->slot[g] == 0) {
        return NULL;
    }
    return &a->recs[a->slot[g] - 1];
}

/* The caller has reserved both a record and a slot for g. */
static op_group *add_rec(ps_allocator *a, u32 g, bool is_new, const u8 *bits)
{
    PS_ASSERT(a->nrecs < a->recs_cap && g < a->cap && a->slot[g] == 0);
    op_group *r = &a->recs[a->nrecs++];
    r->g = g;
    r->is_new = is_new;
    r->changed = false;
    r->bits = bits;
    r->net = 0;
    a->slot[g] = a->nrecs;
    return r;
}

/* Fetch the committed bitmap on the first lookup of an existing group. */
static int get_rec(
    ps_allocator *a,
    ps_bitmap_get_fn get,
    ps_bitmap_get_ctx *ctx,
    u32 g,
    op_group **out)
{
    op_group *r = find_rec(a, g);
    if (r == NULL) {
        int rv = reserve_recs(a);
        if (rv != 0) {
            return rv;
        }
        const u8 *bits = NULL;
        rv = get(ctx, g, &bits);
        if (rv != 0) {
            return rv;
        }
        r = add_rec(a, g, false, bits);
    }
    *out = r;
    return 0;
}

/* Reserve a zeroed delta before adding the group to the changed list. */
static int prepare_change(ps_allocator *a, op_group *r)
{
    if (r->changed) {
        return 0;
    }
    if (r->delta == NULL) {
        r->delta = calloc(1, delta_bytes(a));
        if (r->delta == NULL) {
            return ENOMEM;
        }
    } else {
        memset(r->delta, 0, delta_bytes(a));
    }
    u32 i = a->nchanged;
    /* The caller applies changed groups in ascending order. */
    while (i > 0 && a->changed[i - 1] > r->g) {
        a->changed[i] = a->changed[i - 1];
        i--;
    }
    a->changed[i] = r->g;
    a->nchanged++;
    r->changed = true;
    return 0;
}

/* Add a group to this operation. Commit publishes it. */
static int new_group(ps_allocator *a, op_group **out)
{
    u32 g = a->op_num_groups;
    int rv = reserve_groups(a, g + 1);
    if (rv != 0) {
        return rv;
    }
    rv = reserve_recs(a);
    if (rv != 0) {
        return rv;
    }
    op_group *r = add_rec(a, g, true, NULL);
    rv = prepare_change(a, r);
    if (rv != 0) {
        /* Keep the slot's buffer for reuse. */
        a->slot[g] = 0;
        a->nrecs--;
        return rv;
    }
    a->op_num_groups++;
    *out = r;
    return 0;
}

void ps_allocator_set_group_limit(ps_allocator *a, u32 n)
{
    PS_ASSERT(!a->in_op);
    a->group_limit = n < a->max_groups ? n : a->max_groups;
}

/* ---- allocation and free ---- */

int ps_allocator_next(
    ps_allocator *a,
    ps_bitmap_get_fn get,
    ps_bitmap_get_ctx *ctx,
    ps_page_id *id,
    bool *new_group_out)
{
    PS_ASSERT(a->in_op);
    u32 g;
    u32 from;

    if (a->has_last) {
        u64 next = (u64) a->last.raw + 1;
        g = (u32) (next / a->gsize);
        from = (u32) (next % a->gsize);
    } else {
        g = a->cursor;
        from = 0;
    }

    op_group *r = NULL;
    u32 bit = 0;
    bool created = false;

    for (; g < a->op_num_groups; g++, from = 0) {
        u32 nbits = group_nbits(a, g);
        u32 start = from == 0 ? 1 : from;
        if (start >= nbits) {
            continue;
        }
        if (g >= a->num_groups) {
            /* A new group has no committed allocations to scan past. */
            r = find_rec(a, g);
            PS_ASSERT(r != NULL && r->is_new);
            bit = start;
            break;
        }
        if (a->free_count[g] == 0) {
            continue;
        }
        int rv = get_rec(a, get, ctx, g, &r);
        if (rv != 0) {
            return rv;
        }
        /* The search is past this operation's allocations, so the committed bits suffice. */
        bit = ps_bit_find_clear(r->bits, nbits, start);
        if (bit < nbits) {
            break;
        }
        r = NULL;
    }

    if (r == NULL) {
        if (a->op_num_groups >= a->max_groups) {
            return ENOSPC;
        }
        if (a->op_num_groups >= a->group_limit) {
            return ENOBUFS;
        }
        int rv = new_group(a, &r);
        if (rv != 0) {
            return rv;
        }
        created = true;
        bit = 1;
    } else {
        int rv = prepare_change(a, r);
        if (rv != 0) {
            return rv;
        }
    }

    PS_ASSERT(!ps_bit_test(r->delta, bit));
    ps_bit_set(r->delta, bit);
    r->net++;
    a->last = ps_page_id_from((u32) ((u64) r->g * a->gsize + bit));
    a->has_last = true;
    *id = a->last;
    *new_group_out = created;
    return 0;
}

int ps_allocator_free(
    ps_allocator *a,
    ps_bitmap_get_fn get,
    ps_bitmap_get_ctx *ctx,
    ps_page_id id,
    bool *cancelled)
{
    PS_ASSERT(a->in_op);
    if ((u64) id.raw >= PS_ALLOC_ID_LIMIT) {
        return EINVAL;
    }
    u32 g = id.raw / a->gsize;
    u32 bit = id.raw % a->gsize;
    if (bit == 0 || g >= a->op_num_groups) {
        return EINVAL;
    }

    op_group *r = NULL;
    if (g >= a->num_groups) {
        r = find_rec(a, g);
        PS_ASSERT(r != NULL && r->is_new);
    } else {
        int rv = get_rec(a, get, ctx, g, &r);
        if (rv != 0) {
            return rv;
        }
    }

    bool committed = r->bits != NULL && ps_bit_test(r->bits, bit);
    bool flipped = r->changed && ps_bit_test(r->delta, bit);

    if (flipped) {
        if (committed) {
            return EINVAL; /* already freed in this operation */
        }
        /* Cancel the allocation without moving the cursor back. */
        ps_bit_clear(r->delta, bit);
        r->net--;
        *cancelled = true;
        return 0;
    }
    if (!committed) {
        return EINVAL;
    }
    int rv = prepare_change(a, r);
    if (rv != 0) {
        return rv;
    }
    ps_bit_set(r->delta, bit);
    r->net--;
    *cancelled = false;
    return 0;
}

/* ---- applying an operation ---- */

u32 ps_allocator_op_groups(const ps_allocator *a, const u32 **groups)
{
    *groups = a->changed;
    return a->nchanged;
}

bool ps_allocator_group_is_new(const ps_allocator *a, u32 g)
{
    return a->in_op && g >= a->num_groups && g < a->op_num_groups;
}

bool ps_allocator_group_unchanged(const ps_allocator *a, u32 g)
{
    PS_ASSERT(a->in_op);
    const op_group *r = find_rec(a, g);
    PS_ASSERT(r != NULL && r->changed);
    if (r->is_new) {
        return false;
    }
    for (usize off = 0; off < delta_bytes(a); off += 8) {
        u64 d;
        memcpy(&d, r->delta + off, 8);
        if (d != 0) {
            return false;
        }
    }
    return true;
}

void ps_allocator_apply(const ps_allocator *a, u32 g, u8 *bits)
{
    PS_ASSERT(a->in_op);
    const op_group *r = find_rec(a, g);
    PS_ASSERT(r != NULL && r->changed);
    u32 nbits = group_nbits(a, g);

    if (r->is_new) {
        ps_bit_set(bits, 0);
        for (u32 b = nbits; b < a->gsize; b++) {
            ps_bit_set(bits, b);
        }
    }
    /* The bitmap payload is a multiple of 64 bits, so there is no partial word. */
    for (usize off = 0; off < delta_bytes(a); off += 8) {
        u64 d;
        memcpy(&d, r->delta + off, 8);
        if (d != 0) {
            u64 v;
            memcpy(&v, bits + off, 8);
            v ^= d;
            memcpy(bits + off, &v, 8);
        }
    }
}

void ps_allocator_op_commit(ps_allocator *a)
{
    PS_ASSERT(a->in_op);
    i64 total = 0;
    u32 lowest_free = UINT32_MAX;

    for (u32 i = 0; i < a->nchanged; i++) {
        const op_group *r = find_rec(a, a->changed[i]);
        u32 g = r->g;
        if (r->is_new) {
            a->free_count[g] = group_nbits(a, g) - 1;
        }
        PS_ASSERT((i64) a->free_count[g] - r->net >= 0);
        a->free_count[g] = (u32) ((i64) a->free_count[g] - r->net);
        total += r->net;
        if (a->free_count[g] > 0 && g < lowest_free) {
            lowest_free = g;
        }
    }
    a->allocated = (u64) ((i64) a->allocated + total);
    a->num_groups = a->op_num_groups;
    if (lowest_free < a->cursor) {
        a->cursor = lowest_free;
    }
    while (a->cursor + 1 < a->num_groups && a->free_count[a->cursor] == 0) {
        a->cursor++;
    }
    op_reset(a);
    a->in_op = false;
}

void ps_allocator_op_abort(ps_allocator *a)
{
    PS_ASSERT(a->in_op);
    op_reset(a);
    a->in_op = false;
}
