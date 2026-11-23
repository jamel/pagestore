#include "model.h"

#include "allocator.h"
#include "pagestore/compiler.h"
#include "pagestore/pagestore.h"
#include "test.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Valid IDs are below this (src/allocator.h). */
#define M_ID_LIMIT ((u64) UINT32_MAX)

typedef struct m_ver {
    u64 lsn;
    bool freed;
    u8 *bytes; /* caller area; NULL = zero-filled (or freed) */
} m_ver;

typedef enum m_pend_kind {
    P_NONE, /* a cancelled allocation's slot */
    P_ALLOC,
    P_WRITE,
    P_FREE,
} m_pend_kind;

typedef struct m_pend {
    u32 id;
    m_pend_kind kind;
    u8 *bytes; /* P_ALLOC (NULL = still zero) and P_WRITE */
} m_pend;

typedef struct m_page {
    m_ver *v; /* ascending lsn */
    u32 n;
    u32 cap;
    u32 pend; /* index + 1 into model.pend during an operation, else 0 */
} m_page;

typedef struct m_lsn {
    u64 lsn;
    u32 num_groups; /* after this operation */
} m_lsn;

struct model {
    u32 page_size;
    u32 csize;
    u32 gsize;
    u8 *zero; /* csize zero bytes */

    m_page *pages; /* slots for ids < npages */
    u32 npages;
    u64 *alloc; /* committed allocation bit per id < npages (bitmap ids clear) */
    u32 num_groups;
    m_lsn *lsns; /* committed operations, ascending */
    u32 nlsns;
    u32 lsns_cap;
    u64 *ckpts; /* model_durable LSNs, ascending */
    u32 nckpts;
    u32 ckpts_cap;

    bool in_op;
    u64 op_lsn;
    bool has_last;
    u32 last;
    u32 op_num_groups;
    m_pend *pend;
    u32 npend;
    u32 pend_cap;
};

static void *xmalloc(usize n)
{
    void *p = malloc(n == 0 ? 1 : n);
    if (p == NULL) {
        t_fail(__FILE__, __LINE__, "model: out of memory");
    }
    return p;
}

static void *xcalloc(usize n)
{
    void *p = calloc(1, n == 0 ? 1 : n);
    if (p == NULL) {
        t_fail(__FILE__, __LINE__, "model: out of memory");
    }
    return p;
}

static void *xrealloc(void *p, usize n)
{
    void *q = realloc(p, n == 0 ? 1 : n);
    if (q == NULL) {
        t_fail(__FILE__, __LINE__, "model: out of memory");
    }
    return q;
}

model *model_create(u32 page_size)
{
    T_CHECK(page_size > PS_PAGE_HEADER_SIZE);
    model *m = xcalloc(sizeof(*m));
    m->page_size = page_size;
    m->csize = page_size - PS_PAGE_HEADER_SIZE;
    m->gsize = ps_group_size(page_size);
    m->zero = xcalloc(m->csize);
    m->num_groups = 1; /* the format creates group 0 */
    return m;
}

static void drop_pending(model *m)
{
    for (u32 i = 0; i < m->npend; i++) {
        free(m->pend[i].bytes);
        if (m->pend[i].kind != P_NONE) {
            m->pages[m->pend[i].id].pend = 0;
        }
    }
    m->npend = 0;
    m->in_op = false;
}

void model_destroy(model *m)
{
    if (m == NULL) {
        return;
    }
    drop_pending(m);
    for (u32 id = 0; id < m->npages; id++) {
        for (u32 i = 0; i < m->pages[id].n; i++) {
            free(m->pages[id].v[i].bytes);
        }
        free(m->pages[id].v);
    }
    free(m->pages);
    free(m->alloc);
    free(m->lsns);
    free(m->ckpts);
    free(m->pend);
    free(m->zero);
    free(m);
}

u32 model_page_size(const model *m)
{
    return m->page_size;
}

u32 model_caller_size(const model *m)
{
    return m->csize;
}

u32 model_group_size(const model *m)
{
    return m->gsize;
}

static bool is_user_id(const model *m, u32 id)
{
    return (u64) id < M_ID_LIMIT && id % m->gsize != 0;
}

/* Grow version slots and the committed allocation map together. */
static void ensure_page(model *m, u32 id)
{
    if (id < m->npages) {
        return;
    }
    u64 want = m->npages == 0 ? 1024 : (u64) m->npages * 2;
    while (want <= id) {
        want *= 2;
    }
    if (want > M_ID_LIMIT) {
        want = M_ID_LIMIT;
    }
    const u32 n = (u32) want;
    m->pages = xrealloc(m->pages, (usize) n * sizeof(*m->pages));
    memset(m->pages + m->npages, 0, (usize) (n - m->npages) * sizeof(*m->pages));
    const usize old_words = ((usize) m->npages + 63) / 64;
    const usize words = ((usize) n + 63) / 64;
    m->alloc = xrealloc(m->alloc, words * sizeof(*m->alloc));
    memset(m->alloc + old_words, 0, (words - old_words) * sizeof(*m->alloc));
    m->npages = n;
}

static bool committed_alloc(const model *m, u32 id)
{
    return id < m->npages && (m->alloc[id / 64] >> (id % 64) & 1) != 0;
}

static void set_alloc(model *m, u32 id, bool on)
{
    const u64 bit = (u64) 1 << (id % 64);
    if (on) {
        m->alloc[id / 64] |= bit;
    } else {
        m->alloc[id / 64] &= ~bit;
    }
}

/* Returns end if the search runs out of IDs in this group. */
static u64 find_clear(const model *m, u64 from, u64 end)
{
    u64 id = from;
    while (id < end) {
        if (id >= m->npages) {
            return id;
        }
        const u64 word = m->alloc[id / 64] | (((u64) 1 << (id % 64)) - 1);
        if (word != UINT64_MAX) {
            const u64 c = (id & ~(u64) 63) + (u64) PS_CTZ64(~word);
            return c < end ? c : end;
        }
        id = (id & ~(u64) 63) + 64;
    }
    return end;
}

/* Use a local group count when predicting allocations without changing m. */
static u32 next_id(const model *m, bool has_last, u32 last, u32 *groups)
{
    u64 id = has_last ? (u64) last + 1 : 1;
    for (;;) {
        T_CHECK(id < M_ID_LIMIT); /* ENOSPC is not modelled */
        const u32 g = (u32) (id / m->gsize);
        if (g >= *groups) {
            const u32 ng = (*groups)++;
            return ng * m->gsize + 1;
        }
        if (id % m->gsize == 0) {
            id++;
            continue;
        }
        if (g >= m->num_groups) {
            return (u32) id; /* No committed pages in a new group. */
        }
        u64 end = (u64) (g + 1) * m->gsize;
        if (end > M_ID_LIMIT) {
            end = M_ID_LIMIT;
        }
        const u64 c = find_clear(m, id, end);
        if (c < end) {
            return (u32) c;
        }
        id = end;
    }
}

static const m_ver *latest(const model *m, u32 id)
{
    if (id >= m->npages || m->pages[id].n == 0) {
        return NULL;
    }
    return &m->pages[id].v[m->pages[id].n - 1];
}

static const u8 *ver_bytes(const model *m, const m_ver *v)
{
    return v->bytes != NULL ? v->bytes : m->zero;
}

/* ---- operations ---- */

void model_op_begin(model *m, u64 lsn)
{
    T_CHECK(!m->in_op);
    T_CHECK(lsn > model_last_lsn(m));
    m->in_op = true;
    m->op_lsn = lsn;
    m->has_last = false;
    m->last = 0;
    m->op_num_groups = m->num_groups;
    m->npend = 0;
}

bool model_in_op(const model *m)
{
    return m->in_op;
}

static m_pend *find_pend(const model *m, u32 id)
{
    if (id >= m->npages || m->pages[id].pend == 0) {
        return NULL;
    }
    return &m->pend[m->pages[id].pend - 1];
}

static m_pend *add_pend(model *m, u32 id, m_pend_kind kind, u8 *bytes)
{
    if (m->npend == m->pend_cap) {
        m->pend_cap = m->pend_cap == 0 ? 64 : m->pend_cap * 2;
        m->pend = xrealloc(m->pend, (usize) m->pend_cap * sizeof(*m->pend));
    }
    m_pend *p = &m->pend[m->npend++];
    p->id = id;
    p->kind = kind;
    p->bytes = bytes;
    m->pages[id].pend = m->npend;
    return p;
}

ps_page_id model_alloc(model *m)
{
    T_CHECK(m->in_op);
    const u32 id = next_id(m, m->has_last, m->last, &m->op_num_groups);
    m->has_last = true;
    m->last = id;
    ensure_page(m, id);
    T_CHECK(m->pages[id].pend == 0 && !committed_alloc(m, id));
    add_pend(m, id, P_ALLOC, NULL);
    return ps_page_id_from(id);
}

void model_free(model *m, ps_page_id page)
{
    const u32 id = page.raw;
    T_CHECK(m->in_op);
    T_CHECK(is_user_id(m, id));
    m_pend *p = find_pend(m, id);
    if (p != NULL && p->kind == P_ALLOC) {
        /* Leave the cursor past the cancelled ID until the next operation. */
        free(p->bytes);
        p->bytes = NULL;
        p->kind = P_NONE;
        m->pages[id].pend = 0;
        return;
    }
    if (p != NULL) {
        T_CHECK(p->kind == P_WRITE);
        free(p->bytes);
        p->bytes = NULL;
        p->kind = P_FREE;
        return;
    }
    T_CHECK(committed_alloc(m, id));
    add_pend(m, id, P_FREE, NULL);
}

void model_write(model *m, ps_page_id page, u32 off, const void *data, u32 len)
{
    const u32 id = page.raw;
    T_CHECK(m->in_op);
    T_CHECK(is_user_id(m, id));
    T_CHECK((u64) off + len <= m->csize);
    m_pend *p = find_pend(m, id);
    if (p == NULL) {
        T_CHECK(committed_alloc(m, id));
        u8 *b = xmalloc(m->csize);
        memcpy(b, ver_bytes(m, latest(m, id)), m->csize);
        p = add_pend(m, id, P_WRITE, b);
    }
    T_CHECK(p->kind == P_ALLOC || p->kind == P_WRITE);
    if (p->bytes == NULL) {
        p->bytes = xcalloc(m->csize);
    }
    if (len > 0) {
        memcpy(p->bytes + off, data, len);
    }
}

bool model_op_page(const model *m, ps_page_id page, model_page *out)
{
    const u32 id = page.raw;
    T_CHECK(m->in_op);
    if (!is_user_id(m, id)) {
        return false;
    }
    const m_pend *p = find_pend(m, id);
    if (p != NULL) {
        if (p->kind == P_FREE) {
            return false;
        }
        if (out != NULL) {
            out->caller = p->bytes != NULL ? p->bytes : m->zero;
            out->lsn = m->op_lsn;
        }
        return true;
    }
    if (!committed_alloc(m, id)) {
        return false;
    }
    if (out != NULL) {
        const m_ver *v = latest(m, id);
        out->caller = ver_bytes(m, v);
        out->lsn = v->lsn;
    }
    return true;
}

void model_op_next_ids(const model *m, u32 n, ps_page_id *ids)
{
    T_CHECK(m->in_op);
    bool has_last = m->has_last;
    u32 last = m->last;
    u32 groups = m->op_num_groups;
    for (u32 i = 0; i < n; i++) {
        last = next_id(m, has_last, last, &groups);
        has_last = true;
        ids[i] = ps_page_id_from(last);
    }
}

static void push_version(model *m, u32 id, u64 lsn, bool freed, u8 *bytes)
{
    m_page *pg = &m->pages[id];
    T_CHECK(pg->n == 0 || pg->v[pg->n - 1].lsn < lsn);
    if (pg->n == pg->cap) {
        pg->cap = pg->cap == 0 ? 2 : pg->cap * 2;
        pg->v = xrealloc(pg->v, (usize) pg->cap * sizeof(*pg->v));
    }
    pg->v[pg->n].lsn = lsn;
    pg->v[pg->n].freed = freed;
    pg->v[pg->n].bytes = bytes;
    pg->n++;
}

void model_op_commit(model *m)
{
    T_CHECK(m->in_op);
    for (u32 i = 0; i < m->npend; i++) {
        m_pend *p = &m->pend[i];
        if (p->kind == P_NONE) {
            continue;
        }
        const bool freed = p->kind == P_FREE;
        push_version(m, p->id, m->op_lsn, freed, p->bytes);
        p->bytes = NULL;
        set_alloc(m, p->id, !freed);
        m->pages[p->id].pend = 0;
    }
    m->npend = 0;
    m->num_groups = m->op_num_groups;
    if (m->nlsns == m->lsns_cap) {
        m->lsns_cap = m->lsns_cap == 0 ? 256 : m->lsns_cap * 2;
        m->lsns = xrealloc(m->lsns, (usize) m->lsns_cap * sizeof(*m->lsns));
    }
    m->lsns[m->nlsns].lsn = m->op_lsn;
    m->lsns[m->nlsns].num_groups = m->num_groups;
    m->nlsns++;
    m->in_op = false;
}

void model_op_abort(model *m)
{
    T_CHECK(m->in_op);
    drop_pending(m);
}

/* ---- committed history ---- */

static const m_ver *visible(const model *m, u64 lsn, u32 id)
{
    if (id >= m->npages) {
        return NULL;
    }
    const m_page *pg = &m->pages[id];
    for (u32 i = pg->n; i-- > 0;) {
        if (pg->v[i].lsn <= lsn) {
            return &pg->v[i];
        }
    }
    return NULL;
}

bool model_page_at(const model *m, u64 lsn, ps_page_id id, model_page *out)
{
    const m_ver *v = visible(m, lsn, id.raw);
    if (v == NULL || v->freed) {
        return false;
    }
    if (out != NULL) {
        out->caller = ver_bytes(m, v);
        out->lsn = v->lsn;
    }
    return true;
}

u32 model_allocated_at(const model *m, u64 lsn, ps_page_id *ids, u32 cap)
{
    u32 n = 0;
    for (u32 id = 1; id < m->npages; id++) {
        if (m->pages[id].n > 0 && model_page_at(m, lsn, ps_page_id_from(id), NULL)) {
            if (n < cap) {
                ids[n] = ps_page_id_from(id);
            }
            n++;
        }
    }
    return n;
}

u32 model_num_groups(const model *m)
{
    return m->num_groups;
}

bool model_has_lsn(const model *m, u64 lsn)
{
    if (lsn == 0) {
        return true;
    }
    u32 lo = 0;
    u32 hi = m->nlsns;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2;
        if (m->lsns[mid].lsn < lsn) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < m->nlsns && m->lsns[lo].lsn == lsn;
}

u64 model_last_lsn(const model *m)
{
    return m->nlsns == 0 ? 0 : m->lsns[m->nlsns - 1].lsn;
}

void model_truncate(model *m, u64 lsn)
{
    T_CHECK(!m->in_op);
    for (u32 id = 0; id < m->npages; id++) {
        m_page *pg = &m->pages[id];
        if (pg->n == 0 || pg->v[pg->n - 1].lsn <= lsn) {
            continue;
        }
        while (pg->n > 0 && pg->v[pg->n - 1].lsn > lsn) {
            pg->n--;
            free(pg->v[pg->n].bytes);
        }
        set_alloc(m, id, pg->n > 0 && !pg->v[pg->n - 1].freed);
    }
    while (m->nlsns > 0 && m->lsns[m->nlsns - 1].lsn > lsn) {
        m->nlsns--;
    }
    m->num_groups = m->nlsns == 0 ? 1 : m->lsns[m->nlsns - 1].num_groups;
    while (m->nckpts > 0 && m->ckpts[m->nckpts - 1] > lsn) {
        m->nckpts--;
    }
}

void model_next_ids(const model *m, u32 n, ps_page_id *ids)
{
    bool has_last = false;
    u32 last = 0;
    u32 groups = m->num_groups;
    for (u32 i = 0; i < n; i++) {
        last = next_id(m, has_last, last, &groups);
        has_last = true;
        ids[i] = ps_page_id_from(last);
    }
}

/* ---- durable state and snapshot misuse ---- */

void model_durable(model *m, u64 lsn)
{
    T_CHECK(lsn <= model_last_lsn(m));
    if (m->nckpts > 0) {
        T_CHECK(lsn >= m->ckpts[m->nckpts - 1]);
        if (lsn == m->ckpts[m->nckpts - 1]) {
            return;
        }
    }
    if (m->nckpts == m->ckpts_cap) {
        m->ckpts_cap = m->ckpts_cap == 0 ? 64 : m->ckpts_cap * 2;
        m->ckpts = xrealloc(m->ckpts, (usize) m->ckpts_cap * sizeof(*m->ckpts));
    }
    m->ckpts[m->nckpts++] = lsn;
}

bool model_misuse_ok(const model *m, u64 lsn, ps_page_id id, int rv, u64 page_lsn, const u8 *caller)
{
    if (!is_user_id(m, id.raw)) {
        return rv == EINVAL;
    }
    T_CHECK(!model_page_at(m, lsn, id, NULL));
    if (rv == EINVAL) {
        return true;
    }
    /*
     * A checkpointed free leaves the old bytes on disk, so accept a copy
     * from any checkpoint at which the page was allocated.
     */
    bool written = false;
    for (u32 i = 0; i < m->nckpts; i++) {
        model_page mp;
        if (!model_page_at(m, m->ckpts[i], id, &mp)) {
            continue;
        }
        written = true;
        if (rv == 0 && mp.lsn == page_lsn && page_lsn <= lsn &&
            memcmp(mp.caller, caller, m->csize) == 0) {
            return true;
        }
    }
    return rv == EBADMSG && !written;
}
