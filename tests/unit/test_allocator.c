#include "allocator.h"
#include "bitmap.h"
#include "test.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define G4K 32576u

/* Full groups share a bitmap so the fixture can cover the whole ID space. */
struct ps_bitmap_get_ctx {
    u32 page_size;
    u32 gsize;
    u32 max_groups;
    u8 **bits; /* max_groups slots, owned */
    const u8 *shared;
    u32 *calls; /* per group, reset at fx_begin */
    int fail; /* non-zero: get returns it */
};

typedef struct fx {
    ps_allocator *a;
    ps_bitmap_get_ctx ctx;
    bool last_new;
} fx;

static int fake_get(ps_bitmap_get_ctx *ctx, u32 group, const u8 **bits)
{
    T_CHECK(group < ctx->max_groups);
    if (ctx->fail != 0) {
        return ctx->fail;
    }
    ctx->calls[group]++;
    T_EQ(ctx->calls[group], 1);
    *bits = ctx->bits[group] != NULL ? ctx->bits[group] : ctx->shared;
    T_CHECK(*bits != NULL);
    return 0;
}

static usize fx_bytes(const fx *f)
{
    return f->ctx.gsize / 8;
}

static void fx_open(fx *f, u32 page_size)
{
    memset(f, 0, sizeof(*f));
    T_OK(ps_allocator_create(page_size, &f->a));
    f->ctx.page_size = page_size;
    f->ctx.gsize = ps_group_size(page_size);
    f->ctx.max_groups = ps_max_groups(page_size);
    f->ctx.bits = calloc(f->ctx.max_groups, sizeof(*f->ctx.bits));
    f->ctx.calls = calloc(f->ctx.max_groups, sizeof(*f->ctx.calls));
    T_CHECK(f->ctx.bits != NULL && f->ctx.calls != NULL);
}

static void fx_close(fx *f)
{
    ps_allocator_destroy(f->a);
    for (u32 g = 0; g < f->ctx.max_groups; g++) {
        free(f->ctx.bits[g]);
    }
    free(f->ctx.bits);
    free(f->ctx.calls);
}

/* Registers a full group or one with only its own bitmap bit set. */
static u8 *fx_add_group(fx *f, bool full)
{
    u32 g = ps_allocator_num_groups(f->a);
    u8 *b = malloc(fx_bytes(f));
    T_CHECK(b != NULL);
    memset(b, full ? 0xFF : 0x00, fx_bytes(f));
    ps_bit_set(b, 0);
    f->ctx.bits[g] = b;
    T_OK(ps_allocator_init_group(f->a, g, b));
    return b;
}

static void fx_begin(fx *f)
{
    memset(f->ctx.calls, 0, f->ctx.max_groups * sizeof(*f->ctx.calls));
    ps_allocator_op_begin(f->a);
}

static u32 fx_next(fx *f)
{
    u32 id = 0;
    T_OK(ps_allocator_next(f->a, fake_get, &f->ctx, &id, &f->last_new));
    return id;
}

static bool fx_free(fx *f, u32 id)
{
    bool cancelled = false;
    T_OK(ps_allocator_free(f->a, fake_get, &f->ctx, id, &cancelled));
    return cancelled;
}

static int fx_free_err(fx *f, u32 id)
{
    bool cancelled = false;
    return ps_allocator_free(f->a, fake_get, &f->ctx, id, &cancelled);
}

/* Apply changes to the fixture bitmaps before committing allocator state. */
static void fx_commit(fx *f)
{
    const u32 *groups = NULL;
    u32 n = ps_allocator_op_groups(f->a, &groups);

    for (u32 i = 0; i < n; i++) {
        u32 g = groups[i];
        if (i > 0) {
            T_CHECK(groups[i - 1] < g);
        }
        if (ps_allocator_group_is_new(f->a, g)) {
            T_CHECK(f->ctx.bits[g] == NULL);
            f->ctx.bits[g] = calloc(1, fx_bytes(f));
            T_CHECK(f->ctx.bits[g] != NULL);
        }
        T_CHECK(f->ctx.bits[g] != NULL);
        ps_allocator_apply(f->a, g, f->ctx.bits[g]);
    }
    ps_allocator_op_commit(f->a);
}

/* Allocate all n IDs in one operation so they share the same cursor. */
static void fx_alloc_n(fx *f, u32 n)
{
    fx_begin(f);
    for (u32 i = 0; i < n; i++) {
        fx_next(f);
    }
    fx_commit(f);
}

static void test_sizes(void)
{
    T_EQ(ps_group_size(4096), 32576);
    T_EQ(ps_group_size(65536), 524096);
    T_EQ(ps_max_groups(4096), 131845);
    T_EQ(ps_max_groups(65536), 8196);
    T_EQ(ps_group_size(8192) % 64, 0);

    ps_allocator *a = NULL;
    T_ERR(ps_allocator_create(4097, &a), EINVAL);
    T_ERR(ps_allocator_create(2048, &a), EINVAL);
    T_ERR(ps_allocator_create(131072, &a), EINVAL);
    T_CHECK(a == NULL);
}

static void test_init_group(void)
{
    fx f;
    fx_open(&f, 4096);
    T_EQ(ps_allocator_num_groups(f.a), 0);

    u8 *b = calloc(1, fx_bytes(&f));
    T_CHECK(b != NULL);
    /* Bit 0 belongs to the bitmap itself. */
    T_ERR(ps_allocator_init_group(f.a, 0, b), EINVAL);
    ps_bit_set(b, 0);
    ps_bit_set(b, 3);
    ps_bit_set(b, 7);
    T_ERR(ps_allocator_init_group(f.a, 1, b), EINVAL);
    T_OK(ps_allocator_init_group(f.a, 0, b));
    T_EQ(ps_allocator_num_groups(f.a), 1);
    T_EQ(ps_allocator_allocated(f.a), 2);
    T_ERR(ps_allocator_init_group(f.a, 0, b), EINVAL);
    f.ctx.bits[0] = b;

    fx_begin(&f);
    T_EQ(fx_next(&f), 1);
    T_EQ(fx_next(&f), 2);
    T_EQ(fx_next(&f), 4);
    T_CHECK(!f.last_new);
    ps_allocator_op_abort(f.a);
    T_EQ(ps_allocator_allocated(f.a), 2);
    fx_close(&f);
}

static void test_fresh_first_ids(void)
{
    fx f;
    fx_open(&f, 4096);
    u8 *b = fx_add_group(&f, false);
    T_EQ(ps_allocator_allocated(f.a), 0);

    fx_begin(&f);
    T_EQ(fx_next(&f), 1);
    T_CHECK(!f.last_new);
    T_EQ(fx_next(&f), 2);
    T_EQ(fx_next(&f), 3);
    T_CHECK(!ps_bit_test(b, 1));
    T_EQ(ps_allocator_allocated(f.a), 0);

    const u32 *groups = NULL;
    T_EQ(ps_allocator_op_groups(f.a, &groups), 1);
    T_EQ(groups[0], 0);
    T_CHECK(!ps_allocator_group_is_new(f.a, 0));
    fx_commit(&f);
    T_EQ(ps_allocator_allocated(f.a), 3);
    T_EQ(ps_bit_count_set(b, G4K), 4);
    T_CHECK(ps_bit_test(b, 0) && ps_bit_test(b, 1) && ps_bit_test(b, 2) && ps_bit_test(b, 3));

    fx_begin(&f);
    T_EQ(ps_allocator_op_groups(f.a, &groups), 0);
    T_EQ(fx_next(&f), 4);
    fx_commit(&f);
    T_EQ(ps_allocator_allocated(f.a), 4);
    T_EQ(ps_allocator_num_groups(f.a), 1);
    fx_close(&f);
}

static void test_lowest_first_after_free(void)
{
    fx f;
    fx_open(&f, 4096);
    fx_add_group(&f, false);
    fx_alloc_n(&f, 5);

    fx_begin(&f);
    T_CHECK(!fx_free(&f, 2));
    T_CHECK(!fx_free(&f, 4));
    fx_commit(&f);
    T_EQ(ps_allocator_allocated(f.a), 3);

    fx_begin(&f);
    T_EQ(fx_next(&f), 2);
    T_EQ(fx_next(&f), 4);
    T_EQ(fx_next(&f), 6);
    fx_commit(&f);
    T_EQ(ps_allocator_allocated(f.a), 6);
    fx_close(&f);
}

static void test_freed_in_op_not_reusable(void)
{
    fx f;
    fx_open(&f, 4096);
    u8 *b = fx_add_group(&f, false);
    fx_alloc_n(&f, 3);

    fx_begin(&f);
    T_CHECK(!fx_free(&f, 1));
    T_EQ(fx_next(&f), 4);
    fx_commit(&f);
    T_CHECK(!ps_bit_test(b, 1));
    T_CHECK(ps_bit_test(b, 4));
    T_EQ(ps_allocator_allocated(f.a), 3);

    fx_begin(&f);
    T_EQ(fx_next(&f), 1);
    T_EQ(fx_next(&f), 5);
    ps_allocator_op_abort(f.a);

    /* Retry the same operation to check allocation order after abort. */
    fx_begin(&f);
    T_EQ(fx_next(&f), 1);
    T_EQ(fx_next(&f), 5);
    ps_allocator_op_abort(f.a);
    T_EQ(ps_allocator_allocated(f.a), 3);
    fx_close(&f);
}

static void test_increasing_within_op(void)
{
    fx f;
    fx_open(&f, 4096);
    fx_add_group(&f, false);
    fx_alloc_n(&f, 10);
    fx_begin(&f);
    fx_free(&f, 5);
    fx_free(&f, 9);
    fx_commit(&f);

    fx_begin(&f);
    T_EQ(fx_next(&f), 5);
    T_EQ(fx_next(&f), 9);
    T_EQ(fx_next(&f), 11);
    fx_commit(&f);

    fx_begin(&f);
    fx_free(&f, 3);
    fx_free(&f, 7);
    fx_commit(&f);
    fx_begin(&f);
    T_EQ(fx_next(&f), 3);
    T_EQ(fx_next(&f), 7);
    T_CHECK(!fx_free(&f, 2));
    T_EQ(fx_next(&f), 12);
    fx_commit(&f);
    T_EQ(ps_allocator_allocated(f.a), 11);
    fx_close(&f);
}

static void test_cancel_alloc(void)
{
    fx f;
    fx_open(&f, 4096);
    u8 *b = fx_add_group(&f, false);

    fx_begin(&f);
    T_EQ(fx_next(&f), 1);
    T_CHECK(fx_free(&f, 1));
    T_EQ(fx_next(&f), 2);
    fx_commit(&f);
    T_EQ(ps_allocator_allocated(f.a), 1);
    T_CHECK(!ps_bit_test(b, 1));
    T_CHECK(ps_bit_test(b, 2));
    T_EQ(ps_bit_count_set(b, G4K), 2);

    fx_begin(&f);
    T_EQ(fx_next(&f), 1);
    T_EQ(fx_next(&f), 3);
    fx_commit(&f);
    T_EQ(ps_allocator_allocated(f.a), 3);
    fx_close(&f);
}

static void test_free_invalid(void)
{
    fx f;
    fx_open(&f, 4096);
    u8 *b = fx_add_group(&f, false);
    fx_alloc_n(&f, 3);

    fx_begin(&f);
    T_ERR(fx_free_err(&f, 0), EINVAL);
    T_ERR(fx_free_err(&f, G4K), EINVAL);
    T_ERR(fx_free_err(&f, G4K + 1), EINVAL); /* user page in an uncreated group */
    T_ERR(fx_free_err(&f, UINT32_MAX), EINVAL);
    T_ERR(fx_free_err(&f, 4), EINVAL);
    T_ERR(fx_free_err(&f, G4K - 1), EINVAL);
    T_CHECK(!fx_free(&f, 2));
    T_ERR(fx_free_err(&f, 2), EINVAL);
    T_EQ(fx_next(&f), 4);
    T_CHECK(fx_free(&f, 4));
    T_ERR(fx_free_err(&f, 4), EINVAL);
    fx_commit(&f);
    T_EQ(ps_allocator_allocated(f.a), 2);
    T_CHECK(ps_bit_test(b, 1) && !ps_bit_test(b, 2) && ps_bit_test(b, 3) && !ps_bit_test(b, 4));
    fx_close(&f);
}

static void test_get_error(void)
{
    fx f;
    fx_open(&f, 4096);
    fx_add_group(&f, false);
    fx_alloc_n(&f, 2);

    fx_begin(&f);
    f.ctx.fail = EIO;
    u32 id = 0;
    bool flag = false;
    T_ERR(ps_allocator_next(f.a, fake_get, &f.ctx, &id, &flag), EIO);
    T_ERR(ps_allocator_free(f.a, fake_get, &f.ctx, 1, &flag), EIO);
    const u32 *groups = NULL;
    T_EQ(ps_allocator_op_groups(f.a, &groups), 0);
    f.ctx.fail = 0;
    T_EQ(fx_next(&f), 3);
    T_CHECK(!fx_free(&f, 1));
    fx_commit(&f);
    T_EQ(ps_allocator_allocated(f.a), 2);
    fx_close(&f);
}

static void test_new_group(void)
{
    fx f;
    fx_open(&f, 4096);
    fx_add_group(&f, true);
    T_EQ(ps_allocator_allocated(f.a), G4K - 1);

    fx_begin(&f);
    T_EQ(fx_next(&f), G4K + 1);
    T_CHECK(f.last_new);
    T_EQ(f.ctx.calls[0], 0); /* full group: no bitmap needed */
    T_EQ(ps_allocator_num_groups(f.a), 1);
    T_CHECK(ps_allocator_group_is_new(f.a, 1));
    T_CHECK(!ps_allocator_group_is_new(f.a, 0));
    T_EQ(fx_next(&f), G4K + 2);
    T_CHECK(!f.last_new);
    ps_allocator_op_abort(f.a);
    T_EQ(ps_allocator_num_groups(f.a), 1);
    T_CHECK(!ps_allocator_group_is_new(f.a, 1));

    fx_begin(&f);
    T_EQ(fx_next(&f), G4K + 1);
    T_CHECK(f.last_new);
    const u32 *groups = NULL;
    T_EQ(ps_allocator_op_groups(f.a, &groups), 1);
    T_EQ(groups[0], 1);
    fx_commit(&f);
    T_EQ(ps_allocator_num_groups(f.a), 2);
    T_EQ(ps_allocator_allocated(f.a), G4K);

    fx_begin(&f);
    T_EQ(fx_next(&f), G4K + 2);
    T_CHECK(!f.last_new);
    T_EQ(f.ctx.calls[1], 1);
    fx_commit(&f);
    fx_close(&f);
}

static void test_new_groups_in_one_op(void)
{
    fx f;
    fx_open(&f, 4096);
    fx_add_group(&f, false);

    fx_begin(&f);
    u32 created = 0;
    u32 prev = 0;
    for (u32 i = 0; i < 2 * (G4K - 1) + 1; i++) {
        u32 id = fx_next(&f);
        T_CHECK(id > prev);
        T_NE(id % G4K, 0);
        prev = id;
        created += f.last_new ? 1 : 0;
    }
    T_EQ(prev, 2 * G4K + 1);
    T_EQ(created, 2);
    T_CHECK(fx_free(&f, G4K + 5));
    const u32 *groups = NULL;
    T_EQ(ps_allocator_op_groups(f.a, &groups), 3);
    fx_commit(&f);
    T_EQ(ps_allocator_num_groups(f.a), 3);
    T_EQ(ps_allocator_allocated(f.a), 2 * (G4K - 1));
    T_CHECK(ps_bit_test(f.ctx.bits[1], 0));
    T_CHECK(!ps_bit_test(f.ctx.bits[1], 5));
    T_EQ(ps_bit_count_set(f.ctx.bits[1], G4K), G4K - 1);

    fx_begin(&f);
    T_EQ(fx_next(&f), G4K + 5);
    T_EQ(fx_next(&f), 2 * G4K + 2);
    fx_commit(&f);
    fx_close(&f);
}

static void test_apply_new_group(void)
{
    fx f;
    fx_open(&f, 4096);
    fx_add_group(&f, true);

    fx_begin(&f);
    T_EQ(fx_next(&f), G4K + 1);
    T_EQ(fx_next(&f), G4K + 2);
    T_EQ(fx_next(&f), G4K + 3);
    T_CHECK(fx_free(&f, G4K + 2));

    u8 *b = calloc(1, fx_bytes(&f));
    T_CHECK(b != NULL);
    ps_allocator_apply(f.a, 1, b);
    T_EQ(ps_bit_count_set(b, G4K), 3);
    T_CHECK(ps_bit_test(b, 0) && ps_bit_test(b, 1) && !ps_bit_test(b, 2) && ps_bit_test(b, 3));
    free(b);
    ps_allocator_op_abort(f.a);
    fx_close(&f);
}

static void test_last_group_tail(void)
{
    fx f;
    fx_open(&f, 65536);
    const u32 gs = f.ctx.gsize;
    const u32 mg = f.ctx.max_groups;
    const u32 tail = (u32) (UINT32_MAX - (u64) (mg - 1) * gs);
    u8 *ones = malloc(fx_bytes(&f));
    T_CHECK(ones != NULL);
    memset(ones, 0xFF, fx_bytes(&f));
    f.ctx.shared = ones;
    for (u32 g = 0; g + 1 < mg; g++) {
        T_OK(ps_allocator_init_group(f.a, g, ones));
    }

    fx_begin(&f);
    u32 first = fx_next(&f);
    T_EQ(first, (mg - 1) * gs + 1);
    T_CHECK(f.last_new);
    T_CHECK(fx_free(&f, first));
    u8 *b = calloc(1, fx_bytes(&f));
    T_CHECK(b != NULL);
    ps_allocator_apply(f.a, mg - 1, b);
    T_EQ(gs - ps_bit_count_set(b, gs), tail - 1);
    T_CHECK(ps_bit_test(b, 0));
    T_EQ(ps_bit_find_clear(b, gs, 1), 1);
    T_EQ(ps_bit_find_clear(b, gs, tail), gs); /* reserved IDs */
    free(b);
    fx_commit(&f);
    T_EQ(ps_allocator_num_groups(f.a), mg);

    fx_begin(&f);
    u32 id = 0;
    for (u32 i = 0; i < tail - 1; i++) {
        id = fx_next(&f);
    }
    T_EQ(id, UINT32_MAX - 1);
    bool nw = false;
    T_ERR(ps_allocator_next(f.a, fake_get, &f.ctx, &id, &nw), ENOSPC);
    fx_commit(&f);
    fx_begin(&f);
    T_ERR(ps_allocator_next(f.a, fake_get, &f.ctx, &id, &nw), ENOSPC);
    T_CHECK(!fx_free(&f, UINT32_MAX - 1));
    /* Allocation uses the committed bitmap until commit. */
    T_ERR(ps_allocator_next(f.a, fake_get, &f.ctx, &id, &nw), ENOSPC);
    fx_commit(&f);
    fx_begin(&f);
    T_EQ(fx_next(&f), UINT32_MAX - 1);
    T_CHECK(!f.last_new);
    ps_allocator_op_abort(f.a);

    f.ctx.shared = NULL;
    fx_close(&f);
    free(ones);
}

static void test_enospc(void)
{
    fx f;
    fx_open(&f, 65536);
    u8 *ones = malloc(fx_bytes(&f));
    T_CHECK(ones != NULL);
    memset(ones, 0xFF, fx_bytes(&f));
    f.ctx.shared = ones;
    for (u32 g = 0; g < f.ctx.max_groups; g++) {
        T_OK(ps_allocator_init_group(f.a, g, ones));
    }
    T_ERR(ps_allocator_init_group(f.a, f.ctx.max_groups, ones), EINVAL);
    T_EQ(ps_allocator_num_groups(f.a), 8196);

    fx_begin(&f);
    u32 id = 0;
    bool nw = false;
    T_ERR(ps_allocator_next(f.a, fake_get, &f.ctx, &id, &nw), ENOSPC);
    const u32 *groups = NULL;
    T_EQ(ps_allocator_op_groups(f.a, &groups), 0);
    ps_allocator_op_abort(f.a);
    fx_close(&f);
    free(ones);
}

static void test_cursor_regression(void)
{
    fx f;
    fx_open(&f, 4096);
    fx_add_group(&f, true);
    fx_add_group(&f, true);
    fx_add_group(&f, true);

    fx_begin(&f);
    T_CHECK(!fx_free(&f, 2 * G4K + 5));
    fx_commit(&f);
    fx_begin(&f);
    T_CHECK(!fx_free(&f, G4K + 7));
    fx_commit(&f);

    fx_begin(&f);
    T_EQ(fx_next(&f), G4K + 7);
    T_EQ(fx_next(&f), 2 * G4K + 5);
    T_EQ(fx_next(&f), 3 * G4K + 1);
    T_CHECK(f.last_new);
    fx_commit(&f);
    T_EQ(ps_allocator_num_groups(f.a), 4);
    fx_close(&f);
}

int main(int argc, char **argv)
{
    t_init(argc, argv);
    T_RUN(test_sizes);
    T_RUN(test_init_group);
    T_RUN(test_fresh_first_ids);
    T_RUN(test_lowest_first_after_free);
    T_RUN(test_freed_in_op_not_reusable);
    T_RUN(test_increasing_within_op);
    T_RUN(test_cancel_alloc);
    T_RUN(test_free_invalid);
    T_RUN(test_get_error);
    T_RUN(test_new_group);
    T_RUN(test_new_groups_in_one_op);
    T_RUN(test_apply_new_group);
    T_RUN(test_last_group_tail);
    T_RUN(test_enospc);
    T_RUN(test_cursor_regression);
    return t_done();
}
