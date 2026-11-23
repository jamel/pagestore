#include "workload.h"

#include "crc32c.h"
#include "test.h"
#include "util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define W_MAGIC 0x31504F57u /* "WOP1" */
#define W_HEAD 20u /* magic, crc, lsn, n */
#define W_ACTION 24u /* kind, 3 zero bytes, id, off, len, fill */

PS_STATIC_ASSERT(W_RECORD_MAX == W_HEAD + W_MAX_ACTIONS * W_ACTION, "W_RECORD_MAX");

u8 w_pattern(u64 fill, u32 off)
{
    u64 x = fill ^ ((u64) (off >> 3) * 0x9E3779B97F4A7C15ull);
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDull;
    x ^= x >> 33;
    x *= 0xC4CEB9FE1A85EC53ull;
    x ^= x >> 33;
    return (u8) (x >> ((off & 7) * 8));
}

void w_pattern_fill(u64 fill, u32 off, u8 *dst, u32 len)
{
    for (u32 i = 0; i < len; i++) {
        dst[i] = w_pattern(fill, off + i);
    }
}

/* ---- operation frame bound ---- */

/* Tracks the operation's actions to bound its ps_pressure.op_frames. */
typedef struct w_bound {
    u32 frames;
    u32 groups[2 * W_MAX_ACTIONS];
    u32 ngroups;
    ps_page_id written[W_MAX_ACTIONS]; /* committed pages copied by a ps_write */
    u32 nwritten;
    ps_page_id allocated[W_MAX_ACTIONS];
    u32 nallocated;
    ps_page_id freed[W_MAX_ACTIONS];
    u32 nfreed;
} w_bound;

static bool in_list(const u32 *list, u32 n, u32 g)
{
    for (u32 i = 0; i < n; i++) {
        if (list[i] == g) {
            return true;
        }
    }
    return false;
}

static bool has_id(const ps_page_id *list, u32 n, ps_page_id id)
{
    for (u32 i = 0; i < n; i++) {
        if (ps_page_id_eq(list[i], id)) {
            return true;
        }
    }
    return false;
}

/* Charge a spare once per existing group and a bitmap plus spare for a new group. */
static u32 group_cost(const w_bound *b, const model *m, ps_page_id id)
{
    const u32 gs = model_group_size(m);
    const u32 g = id.raw / gs;
    if (g >= model_num_groups(m)) {
        return id.raw % gs == 1 ? 2 : 0; /* the bitmap frame and its spare */
    }
    return in_list(b->groups, b->ngroups, g) ? 0 : 1;
}

static void touch_group(w_bound *b, const model *m, ps_page_id id)
{
    const u32 g = id.raw / model_group_size(m);
    if (g < model_num_groups(m) && !in_list(b->groups, b->ngroups, g)) {
        T_CHECK(b->ngroups < 2 * W_MAX_ACTIONS);
        b->groups[b->ngroups++] = g;
    }
}

/* Frames that action a adds to the operation's bound. */
static u32 action_cost(const w_bound *b, const model *m, const w_action *a)
{
    switch (a->kind) {
        case W_ALLOC:
            return 1 + group_cost(b, m, a->id);
        case W_FREE:
            /* A copied page is already charged. A free can still hold its dirty base. */
            return (has_id(b->written, b->nwritten, a->id) ? 0 : 1) + group_cost(b, m, a->id);
        case W_WRITE:
            if (has_id(b->allocated, b->nallocated, a->id) ||
                has_id(b->written, b->nwritten, a->id)) {
                return 0;
            }
            return 2; /* the copy and the committed version under it */
    }
    T_CHECK(false);
    return 0;
}

static void action_take(w_bound *b, const model *m, const w_action *a)
{
    b->frames += action_cost(b, m, a);
    T_CHECK(b->nallocated < W_MAX_ACTIONS && b->nwritten < W_MAX_ACTIONS);
    T_CHECK(b->nfreed < W_MAX_ACTIONS);
    switch (a->kind) {
        case W_ALLOC:
            touch_group(b, m, a->id);
            b->allocated[b->nallocated++] = a->id;
            break;
        case W_FREE:
            touch_group(b, m, a->id);
            b->freed[b->nfreed++] = a->id;
            break;
        case W_WRITE:
            if (!has_id(b->allocated, b->nallocated, a->id) &&
                !has_id(b->written, b->nwritten, a->id)) {
                b->written[b->nwritten++] = a->id;
            }
            break;
    }
}

u32 w_op_frames_bound(const model *m, const w_op *op)
{
    w_bound b;
    memset(&b, 0, sizeof(b));
    for (u32 i = 0; i < op->n; i++) {
        action_take(&b, m, &op->a[i]);
    }
    return b.frames;
}

/* ---- generator ---- */

/* Try a few draws to avoid scanning the whole live set for one ID. */
static ps_page_id pick_live(rng *r, const ps_page_id *live, u32 nlive, const w_bound *b)
{
    for (u32 tries = 0; tries < 8 && nlive > 0; tries++) {
        const ps_page_id id = live[rng_below(r, nlive)];
        if (!has_id(b->freed, b->nfreed, id)) {
            return id;
        }
    }
    return PS_PAGE_ID_NONE;
}

static void gen_write_range(rng *r, const model *m, w_action *a)
{
    const u32 cs = model_caller_size(m);
    const u32 k = (u32) rng_below(r, 10);
    u32 len = k < 5 ? 1 + (u32) rng_below(r, 64) : k < 8 ? 1 + (u32) rng_below(r, 1024) : cs;
    if (len > cs) {
        len = cs;
    }
    a->len = len;
    a->off = (u32) rng_below(r, (u64) cs - len + 1);
    a->fill = rng_next(r);
}

void w_gen(rng *r, const model *m, u64 lsn, u32 max_actions, w_op *out)
{
    T_CHECK(max_actions >= 1 && max_actions <= W_MAX_ACTIONS);
    T_CHECK(lsn > model_last_lsn(m));
    const u64 last = model_last_lsn(m);
    const u32 nlive = model_allocated_at(m, last, NULL, 0);
    ps_page_id *live = malloc(((usize) nlive + 1) * sizeof(*live));
    T_CHECK(live != NULL);
    model_allocated_at(m, last, live, nlive);
    ps_page_id next[W_MAX_ACTIONS];
    model_next_ids(m, max_actions, next);

    memset(out, 0, sizeof(*out));
    out->lsn = lsn;
    w_bound b;
    memset(&b, 0, sizeof(b));
    const u32 target = 1 + (u32) rng_below(r, max_actions);
    for (u32 attempt = 0; out->n < target && attempt < 4 * target; attempt++) {
        const u32 now = nlive + b.nallocated - b.nfreed;
        const u32 pa = now < W_LIVE_TARGET / 2 ? 50 : now > W_LIVE_TARGET ? 10 : 30;
        const u32 pf = now > W_LIVE_TARGET ? 45 : 20;
        const u32 x = (u32) rng_below(r, 100);
        w_action a;
        memset(&a, 0, sizeof(a));
        if (x < pa) {
            a.kind = W_ALLOC;
            a.id = next[b.nallocated];
        } else if (x < pa + pf) {
            a.kind = W_FREE;
            a.id = pick_live(r, live, nlive, &b);
        } else {
            a.kind = W_WRITE;
            if (b.nallocated > 0 && rng_below(r, 10) < 3) {
                a.id = b.allocated[rng_below(r, b.nallocated)];
            } else {
                a.id = pick_live(r, live, nlive, &b);
            }
            gen_write_range(r, m, &a);
        }
        if (ps_page_id_is_none(a.id) || b.frames + action_cost(&b, m, &a) > W_OP_FRAMES_MAX) {
            continue;
        }
        action_take(&b, m, &a);
        out->a[out->n++] = a;
    }
    if (out->n == 0) {
        /* An empty operation has room for one allocation, even in a new group. */
        w_action a;
        memset(&a, 0, sizeof(a));
        a.kind = W_ALLOC;
        a.id = next[0];
        action_take(&b, m, &a);
        out->a[out->n++] = a;
    }
    T_CHECK(b.frames <= W_OP_FRAMES_MAX);
    free(live);
}

/* ---- encoding ---- */

usize w_encode(const w_op *op, u8 *buf, usize cap)
{
    T_CHECK(op->n <= W_MAX_ACTIONS);
    const usize size = W_HEAD + (usize) op->n * W_ACTION;
    if (size > cap) {
        return size;
    }
    memset(buf, 0, size);
    ps_put_le32(buf, W_MAGIC);
    ps_put_le64(buf + 8, op->lsn);
    ps_put_le32(buf + 16, op->n);
    for (u32 i = 0; i < op->n; i++) {
        u8 *p = buf + W_HEAD + (usize) i * W_ACTION;
        const w_action *a = &op->a[i];
        p[0] = (u8) a->kind;
        ps_put_le32(p + 4, a->id.raw);
        ps_put_le32(p + 8, a->off);
        ps_put_le32(p + 12, a->len);
        ps_put_le64(p + 16, a->fill);
    }
    ps_put_le32(buf + 4, ps_crc32c(0, buf + 8, size - 8));
    return size;
}

int w_decode(const void *buf, usize len, w_op *op)
{
    const u8 *b = buf;
    if (len < W_HEAD || ps_get_le32(b) != W_MAGIC) {
        return EBADMSG;
    }
    const u32 n = ps_get_le32(b + 16);
    if (n > W_MAX_ACTIONS || len != W_HEAD + (usize) n * W_ACTION) {
        return EBADMSG;
    }
    if (ps_get_le32(b + 4) != ps_crc32c(0, b + 8, len - 8)) {
        return EBADMSG;
    }
    memset(op, 0, sizeof(*op));
    op->lsn = ps_get_le64(b + 8);
    op->n = n;
    for (u32 i = 0; i < n; i++) {
        const u8 *p = b + W_HEAD + (usize) i * W_ACTION;
        if (p[0] > W_WRITE || p[1] != 0 || p[2] != 0 || p[3] != 0) {
            return EBADMSG;
        }
        w_action *a = &op->a[i];
        a->kind = (w_kind) p[0];
        a->id = ps_page_id_from(ps_get_le32(p + 4));
        a->off = ps_get_le32(p + 8);
        a->len = ps_get_le32(p + 12);
        a->fill = ps_get_le64(p + 16);
    }
    return 0;
}

/* ---- applying operations ---- */

int w_apply_store_begin(ps_store *s, const w_op *op)
{
    int rv = ps_op_begin(s, op->lsn);
    if (rv != 0) {
        return rv;
    }
    const u32 cs = ps_page_size(s) - PS_PAGE_HEADER_SIZE;
    for (u32 i = 0; i < op->n && rv == 0; i++) {
        const w_action *a = &op->a[i];
        ps_page *p = NULL;
        switch (a->kind) {
            case W_ALLOC: {
                ps_page_id id = PS_PAGE_ID_NONE;
                rv = ps_alloc(s, &id, &p);
                if (rv == 0) {
                    T_EQ(id.raw, a->id.raw);
                    T_EQ(p->id.raw, id.raw);
                    T_EQ(p->lsn, op->lsn);
                }
                break;
            }
            case W_FREE:
                rv = ps_free(s, a->id);
                break;
            case W_WRITE:
                T_CHECK((u64) a->off + a->len <= cs);
                rv = ps_write(s, a->id, &p);
                if (rv == 0) {
                    T_EQ(p->id.raw, a->id.raw);
                    w_pattern_fill(
                        a->fill,
                        a->off,
                        (u8 *) p + PS_PAGE_HEADER_SIZE + a->off,
                        a->len);
                }
                break;
        }
    }
    if (rv != 0) {
        ps_op_abort(s);
    }
    return rv;
}

int w_apply_store(ps_store *s, const w_op *op)
{
    const int rv = w_apply_store_begin(s, op);
    if (rv == 0) {
        ps_op_commit(s);
    }
    return rv;
}

void w_apply_model_begin(model *m, const w_op *op)
{
    model_op_begin(m, op->lsn);
    u8 *buf = NULL;
    for (u32 i = 0; i < op->n; i++) {
        const w_action *a = &op->a[i];
        switch (a->kind) {
            case W_ALLOC:
                T_EQ(model_alloc(m).raw, a->id.raw);
                break;
            case W_FREE:
                model_free(m, a->id);
                break;
            case W_WRITE:
                if (buf == NULL) {
                    buf = malloc(model_caller_size(m));
                    T_CHECK(buf != NULL);
                }
                T_CHECK((u64) a->off + a->len <= model_caller_size(m));
                w_pattern_fill(a->fill, a->off, buf, a->len);
                model_write(m, a->id, a->off, buf, a->len);
                break;
        }
    }
    free(buf);
}

void w_apply_model(model *m, const w_op *op)
{
    w_apply_model_begin(m, op);
    model_op_commit(m);
}

int w_apply_record(ps_apply_ctx *ctx, ps_store *s, u64 lsn, const void *rec, usize len)
{
    PS_UNUSED(ctx);
    w_op op;
    int rv = w_decode(rec, len, &op);
    if (rv != 0) {
        return rv;
    }
    if (op.lsn != lsn) {
        return EBADMSG;
    }
    for (u32 tries = 0;; tries++) {
        rv = w_apply_store(s, &op);
        if (rv != EAGAIN || tries == 3) {
            return rv;
        }
        rv = ps_checkpoint(s);
        if (rv != 0) {
            return rv;
        }
    }
}

/* ---- checking a store ---- */

static int check_read(ps_store *s, ps_snapshot *sn, ps_page_id id, const ps_page **p)
{
    for (u32 tries = 0;; tries++) {
        const int rv = ps_snapshot_read(sn, id, p);
        if (rv != EAGAIN || tries == 3) {
            return rv;
        }
        T_OK(ps_checkpoint(s));
    }
}

void w_check_store(ps_store *s, const model *m)
{
    ps_snapshot *sn = NULL;
    T_OK(ps_snapshot_open(s, &sn));
    const u64 lsn = ps_snapshot_lsn(sn);
    T_EQ(lsn, ps_committed_lsn(s));
    T_EQ(ps_page_size(s), model_page_size(m));

    const u32 n = model_allocated_at(m, lsn, NULL, 0);
    struct ps_stats st;
    ps_stats(s, &st);
    T_EQ(st.allocated_pages, n);
    ps_page_id *ids = malloc(((usize) n + 1) * sizeof(*ids));
    T_CHECK(ids != NULL);
    model_allocated_at(m, lsn, ids, n);
    for (u32 i = 0; i < n; i++) {
        const ps_page_id id = ids[i];
        model_page mp;
        T_CHECK(model_page_at(m, lsn, id, &mp));
        const ps_page *p = NULL;
        T_OK(check_read(s, sn, id, &p));
        T_EQ(p->id.raw, id.raw);
        T_EQ(p->lsn, mp.lsn);
        T_EQ(p->flags, 0);
        T_EQ(p->checksum, 0);
        T_EQ(p->magic, 0);
        T_MEMEQ((const u8 *) p + PS_PAGE_HEADER_SIZE, mp.caller, model_caller_size(m));
        ps_snapshot_unpin(sn, id);
    }
    free(ids);
    const ps_page *p = NULL;
    T_ERR(ps_snapshot_read(sn, ps_page_id_from(0), &p), EINVAL);
    T_ERR(ps_snapshot_read(sn, ps_page_id_from(model_group_size(m)), &p), EINVAL);
    T_ERR(ps_snapshot_read(sn, ps_page_id_from(UINT32_MAX), &p), EINVAL);
    ps_snapshot_close(sn);
}
