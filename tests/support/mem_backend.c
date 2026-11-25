#include "crc32c.h"
#include "format.h"
#include "mem_backend.h"
#include "test.h"

#include "pagestore/bytes.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MB_HDR_PAGE_ID 4
#define MB_HDR_CHECKSUM 8

/* One durable page image. */
typedef struct mb_page_rec {
    struct mb_page_rec *next;
    ps_page_id id;
    u64 written_seq; /* 0 for a page injected with mb_put */
    u8 *data;
} mb_page_rec;

/* A page staged by the running checkpoint. */
typedef struct mb_staged {
    ps_page_id id;
    u8 *data;
} mb_staged;

typedef struct mb_fault {
    int err;
    i32 count; /* -1 = unlimited */
} mb_fault;

/* The backend ctx is embedded in mem_backend, so close() frees nothing. */
struct ps_backend_ctx {
    mem_backend *mb;
};

struct mem_backend {
    struct ps_backend_ctx ctx;
    pthread_mutex_t mu;
    pthread_cond_t cv;

    bool has_header;
    ps_backend_info info;
    bool open;

    mb_page_rec **buckets;
    u32 nbuckets; /* power of two */
    u32 npages;

    bool in_ckpt;
    ps_lsn ckpt_lsn;
    u32 ckpt_ng;
    u32 ckpt_count;
    mb_staged *staged;
    u32 nstaged;

    u64 ckpt_done;
    u64 aborts;
    mb_fault faults[MB_POINTS];
    bool block_writes;
    u32 commit_delay_ms;
    bool hold_reads;
    u32 reads_held;
};

static void *xalloc(usize n)
{
    void *p = calloc(1, n);
    if (p == NULL) {
        T_FAIL("mem_backend: out of memory");
    }
    return p;
}

mem_backend *mb_create(void)
{
    mem_backend *mb = xalloc(sizeof(*mb));
    mb->ctx.mb = mb;
    pthread_mutex_init(&mb->mu, NULL);
    pthread_cond_init(&mb->cv, NULL);
    mb->nbuckets = 256;
    mb->buckets = xalloc(mb->nbuckets * sizeof(*mb->buckets));
    return mb;
}

static void drop_staged(mem_backend *mb)
{
    for (u32 i = 0; i < mb->nstaged; i++) {
        free(mb->staged[i].data);
    }
    free(mb->staged);
    mb->staged = NULL;
    mb->nstaged = 0;
    mb->in_ckpt = false;
}

void mb_destroy(mem_backend *mb)
{
    drop_staged(mb);
    for (u32 b = 0; b < mb->nbuckets; b++) {
        mb_page_rec *r = mb->buckets[b];
        while (r != NULL) {
            mb_page_rec *next = r->next;
            free(r->data);
            free(r);
            r = next;
        }
    }
    free(mb->buckets);
    pthread_cond_destroy(&mb->cv);
    pthread_mutex_destroy(&mb->mu);
    free(mb);
}

static mb_page_rec *find(const mem_backend *mb, ps_page_id id)
{
    for (mb_page_rec *r = mb->buckets[id.raw & (mb->nbuckets - 1)]; r != NULL; r = r->next) {
        if (ps_page_id_eq(r->id, id)) {
            return r;
        }
    }
    return NULL;
}

static void rehash(mem_backend *mb)
{
    const u32 n = mb->nbuckets * 2;
    mb_page_rec **nb = xalloc(n * sizeof(*nb));
    for (u32 b = 0; b < mb->nbuckets; b++) {
        mb_page_rec *r = mb->buckets[b];
        while (r != NULL) {
            mb_page_rec *next = r->next;
            r->next = nb[r->id.raw & (n - 1)];
            nb[r->id.raw & (n - 1)] = r;
            r = next;
        }
    }
    free(mb->buckets);
    mb->buckets = nb;
    mb->nbuckets = n;
}

/* Hold mu. Overwrite the bytes in place to keep mb_page pointers valid. */
static void store_page(mem_backend *mb, ps_page_id id, const void *page, u64 seq)
{
    const u32 ps = mb->info.page_size;
    T_CHECK(ps != 0);
    mb_page_rec *r = find(mb, id);
    if (r == NULL) {
        if (mb->npages >= mb->nbuckets) {
            rehash(mb);
        }
        r = xalloc(sizeof(*r));
        r->id = id;
        r->data = xalloc(ps);
        r->next = mb->buckets[id.raw & (mb->nbuckets - 1)];
        mb->buckets[id.raw & (mb->nbuckets - 1)] = r;
        mb->npages++;
    }
    memcpy(r->data, page, ps);
    r->written_seq = seq;
}

/* Hold mu. */
static int take_fault(mem_backend *mb, mb_point p)
{
    mb_fault *f = &mb->faults[p];
    if (f->err == 0 || f->count == 0) {
        return 0;
    }
    if (f->count > 0) {
        f->count--;
    }
    return f->err;
}

static u32 page_checksum(const u8 *page, u32 page_size)
{
    u8 zero[4] = {0, 0, 0, 0};
    u32 crc = ps_crc32c(0, page, MB_HDR_CHECKSUM);
    crc = ps_crc32c(crc, zero, sizeof(zero));
    return ps_crc32c(crc, page + MB_HDR_CHECKSUM + 4, page_size - MB_HDR_CHECKSUM - 4);
}

void mb_seal_page(void *page, u32 page_size)
{
    u8 *p = page;
    const u32 crc = page_checksum(p, page_size);
    ps_put_le32(p + MB_HDR_CHECKSUM, crc);
}

/* ---- ps_backend callbacks ---- */

static int be_open(ps_backend_ctx *ctx, bool create, u32 page_size, ps_backend_info *out)
{
    mem_backend *mb = ctx->mb;
    pthread_mutex_lock(&mb->mu);
    T_CHECK(!mb->open);
    int rv = 0;
    if (!mb->has_header) {
        if (!create) {
            rv = ENOENT;
        } else if (page_size != 0 && !ps_page_size_valid(page_size)) {
            rv = EINVAL;
        } else {
            mb->info.page_size = page_size == 0 ? 4096 : page_size;
            mb->info.num_groups = 1;
            mb->info.seq = 0;
            mb->info.durable_lsn = PS_LSN_NONE;
            mb->has_header = true;
        }
    }
    if (rv == 0) {
        mb->open = true;
        *out = mb->info;
    }
    pthread_mutex_unlock(&mb->mu);
    return rv;
}

static void sleep_ms(u32 ms)
{
    struct timespec ts = {(time_t) (ms / 1000), (long) (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0) {}
}

static int be_read_page(ps_backend_ctx *ctx, ps_page_id id, void *buf)
{
    mem_backend *mb = ctx->mb;
    pthread_mutex_lock(&mb->mu);
    if (mb->hold_reads) {
        mb->reads_held++;
        pthread_cond_broadcast(&mb->cv);
        while (mb->hold_reads) {
            pthread_cond_wait(&mb->cv, &mb->mu);
        }
        mb->reads_held--;
    }
    int rv = take_fault(mb, MB_READ);
    if (rv == 0) {
        const mb_page_rec *r = find(mb, id);
        if (r != NULL) {
            memcpy(buf, r->data, mb->info.page_size);
        } else {
            memset(buf, 0, mb->info.page_size);
        }
    }
    pthread_mutex_unlock(&mb->mu);
    return rv;
}

static int be_ckpt_begin(ps_backend_ctx *ctx, ps_lsn lsn, u32 num_groups, u32 count)
{
    mem_backend *mb = ctx->mb;
    pthread_mutex_lock(&mb->mu);
    T_CHECK(mb->open && !mb->in_ckpt);
    T_CHECK(num_groups >= 1);
    const int rv = take_fault(mb, MB_BEGIN);
    if (rv != 0) {
        pthread_mutex_unlock(&mb->mu);
        return rv;
    }
    mb->in_ckpt = true;
    mb->ckpt_lsn = lsn;
    mb->ckpt_ng = num_groups;
    mb->ckpt_count = count;
    mb->staged = count == 0 ? NULL : xalloc((usize) count * sizeof(*mb->staged));
    mb->nstaged = 0;
    pthread_mutex_unlock(&mb->mu);
    return 0;
}

static int be_ckpt_write(ps_backend_ctx *ctx, ps_page_id id, const void *page)
{
    mem_backend *mb = ctx->mb;
    const u8 *p = page;
    pthread_mutex_lock(&mb->mu);
    while (mb->block_writes) {
        pthread_cond_wait(&mb->cv, &mb->mu);
    }
    T_CHECK(mb->in_ckpt);
    T_CHECK(mb->nstaged < mb->ckpt_count);
    const u32 pid = ps_get_le32(p + MB_HDR_PAGE_ID);
    const u32 sum = ps_get_le32(p + MB_HDR_CHECKSUM);
    T_EQ(pid, id.raw);
    T_EQ(sum, page_checksum(p, mb->info.page_size));
    const int rv = take_fault(mb, MB_WRITE);
    if (rv == 0) {
        mb_staged *st = &mb->staged[mb->nstaged++];
        st->id = id;
        st->data = xalloc(mb->info.page_size);
        memcpy(st->data, page, mb->info.page_size);
    }
    pthread_mutex_unlock(&mb->mu);
    return rv;
}

static int be_ckpt_commit(ps_backend_ctx *ctx, bool *fatal)
{
    mem_backend *mb = ctx->mb;
    pthread_mutex_lock(&mb->mu);
    const u32 delay = mb->commit_delay_ms;
    pthread_mutex_unlock(&mb->mu);
    if (delay != 0) {
        sleep_ms(delay);
    }

    pthread_mutex_lock(&mb->mu);
    T_CHECK(mb->in_ckpt);
    T_EQ(mb->nstaged, mb->ckpt_count);
    *fatal = false;
    int rv = take_fault(mb, MB_COMMIT_FATAL);
    if (rv != 0) {
        *fatal = true;
    } else {
        rv = take_fault(mb, MB_COMMIT);
    }
    if (rv == 0) {
        const u64 seq = mb->info.seq + 1;
        for (u32 i = 0; i < mb->nstaged; i++) {
            store_page(mb, mb->staged[i].id, mb->staged[i].data, seq);
        }
        mb->info.seq = seq;
        mb->info.durable_lsn = mb->ckpt_lsn;
        mb->info.num_groups = mb->ckpt_ng;
        mb->ckpt_done++;
        drop_staged(mb);
    } else if (*fatal) {
        /* Model a crash before the new header reaches disk. */
        drop_staged(mb);
    }
    pthread_mutex_unlock(&mb->mu);
    return rv;
}

static void be_ckpt_abort(ps_backend_ctx *ctx)
{
    mem_backend *mb = ctx->mb;
    pthread_mutex_lock(&mb->mu);
    /* A fatal commit has already closed the checkpoint. */
    T_CHECK(mb->in_ckpt);
    mb->aborts++;
    drop_staged(mb);
    pthread_mutex_unlock(&mb->mu);
}

static void be_close(ps_backend_ctx *ctx)
{
    mem_backend *mb = ctx->mb;
    pthread_mutex_lock(&mb->mu);
    drop_staged(mb);
    mb->open = false;
    pthread_mutex_unlock(&mb->mu);
}

void mb_backend(mem_backend *mb, ps_backend *out)
{
    out->ctx = &mb->ctx;
    out->open = be_open;
    out->read_page = be_read_page;
    out->ckpt_begin = be_ckpt_begin;
    out->ckpt_write = be_ckpt_write;
    out->ckpt_commit = be_ckpt_commit;
    out->ckpt_abort = be_ckpt_abort;
    out->close = be_close;
}

/* ---- test API ---- */

ps_config mb_config(u32 frames)
{
    ps_config cfg;
    ps_config_init(&cfg);
    cfg.page_size = 4096;
    cfg.cache_size = (usize) frames * 4096;
    cfg.checkpoint_thread = false;
    return cfg;
}

int mb_open(mem_backend *mb, u32 frames, u32 flags, ps_store **out)
{
    const ps_config cfg = mb_config(frames);
    ps_backend be;
    mb_backend(mb, &be);
    return ps_open_backend(&be, &cfg, flags, out);
}

const u8 *mb_page(mem_backend *mb, ps_page_id id)
{
    pthread_mutex_lock(&mb->mu);
    const mb_page_rec *r = find(mb, id);
    const u8 *p = r != NULL ? r->data : NULL;
    pthread_mutex_unlock(&mb->mu);
    return p;
}

void mb_put(mem_backend *mb, ps_page_id id, const void *page)
{
    pthread_mutex_lock(&mb->mu);
    store_page(mb, id, page, 0);
    pthread_mutex_unlock(&mb->mu);
}

void mb_set_info(mem_backend *mb, ps_backend_info info)
{
    pthread_mutex_lock(&mb->mu);
    mb->info = info;
    mb->has_header = true;
    pthread_mutex_unlock(&mb->mu);
}

ps_backend_info mb_info(mem_backend *mb)
{
    pthread_mutex_lock(&mb->mu);
    const ps_backend_info info = mb->info;
    pthread_mutex_unlock(&mb->mu);
    return info;
}

u64 mb_ckpt_count(mem_backend *mb)
{
    pthread_mutex_lock(&mb->mu);
    const u64 n = mb->ckpt_done;
    pthread_mutex_unlock(&mb->mu);
    return n;
}

u64 mb_abort_count(mem_backend *mb)
{
    pthread_mutex_lock(&mb->mu);
    const u64 n = mb->aborts;
    pthread_mutex_unlock(&mb->mu);
    return n;
}

bool mb_written_last(mem_backend *mb, ps_page_id id)
{
    pthread_mutex_lock(&mb->mu);
    const mb_page_rec *r = find(mb, id);
    const bool w = r != NULL && mb->info.seq != 0 && r->written_seq == mb->info.seq;
    pthread_mutex_unlock(&mb->mu);
    return w;
}

void mb_fail(mem_backend *mb, mb_point p, int err, i32 count)
{
    T_CHECK(p < MB_POINTS);
    pthread_mutex_lock(&mb->mu);
    mb->faults[p].err = err;
    mb->faults[p].count = count;
    pthread_mutex_unlock(&mb->mu);
}

void mb_block_writes(mem_backend *mb, bool on)
{
    pthread_mutex_lock(&mb->mu);
    mb->block_writes = on;
    pthread_cond_broadcast(&mb->cv);
    pthread_mutex_unlock(&mb->mu);
}

void mb_set_commit_delay_ms(mem_backend *mb, u32 ms)
{
    pthread_mutex_lock(&mb->mu);
    mb->commit_delay_ms = ms;
    pthread_mutex_unlock(&mb->mu);
}

void mb_hold_reads(mem_backend *mb, bool on)
{
    pthread_mutex_lock(&mb->mu);
    mb->hold_reads = on;
    pthread_cond_broadcast(&mb->cv);
    pthread_mutex_unlock(&mb->mu);
}

void mb_wait_held_reads(mem_backend *mb, u32 n)
{
    pthread_mutex_lock(&mb->mu);
    while (mb->reads_held < n) {
        pthread_cond_wait(&mb->cv, &mb->mu);
    }
    pthread_mutex_unlock(&mb->mu);
}
