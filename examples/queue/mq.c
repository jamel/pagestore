/*
 * Each change to the queue is one store operation with one WAL record.
 * Records are logical, and apply() runs them both live and in recovery.
 *
 *     create | type = 1 | u64 cap |
 *     push   | type = 2 | u32 count | count x (u32 len | bytes) |
 *     sub    | type = 3 | u8 nlen | name | u64 pos |
 *     unsub  | type = 4 | u8 nlen | name |
 *     ack    | type = 5 | u8 nlen | name | u64 pos |
 *
 * A change takes mu, runs and logs its operation at the next LSN and
 * releases mu, then waits for its LSN to be durable. A failed change
 * aborts, and its LSN goes to the next one.
 *
 * Waiting uses group commit. One thread at a time, the leader, flushes the
 * WAL through the newest committed LSN while the others wait on synced.
 * Calling ps_wal_flush from every thread would queue them all on the WAL's
 * I/O lock, even when their records are already durable.
 *
 *     thread A: commit 7 ---- flush through 9 (leader) ----> done
 *     thread B:   commit 8 -- wait ------------------------> done
 *     thread C:     commit 9 -- wait ----------------------> done
 *
 * Reads take no lock. Commit happens before the flush, so a snapshot can
 * hold messages that a crash would still lose. Readers wait on synced for
 * durable_seq, which the leader raises after each flush, to pass their
 * position.
 *
 * Locks: mu guards next_lsn, next_seq, rec and the list of open readers.
 * sync_mu guards the rest of the durability state. Code holding mu may
 * take sync_mu, never the other way around.
 */

#include "mq.h"

#include "pagestore/bytes.h"
#include "pagestore/wal.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// IWYU pragma: no_forward_declare timespec

/*
 * The store has no place to record the log's meta page ID, so mq uses a
 * fixed one. In a new store page 0 holds the allocator's bitmap, so the
 * first allocation gets page 1. Creating the log checks this.
 */
#define MQ_META_ID PS_LITERAL(ps_page_id, 1)

#define REC_CREATE 1u
#define REC_PUSH 2u
#define REC_SUB 3u
#define REC_UNSUB 4u
#define REC_ACK 5u

#define REC_CREATE_SIZE 9u
#define REC_PUSH_HEADER 5u
#define REC_MSG_HEADER 4u
#define REC_READER_HEADER 2u

#define WAL_MIN_SEGMENT ((u64) 4 << 20)

struct mq {
    ps_store *store;
    ps_wal *wal;
    ml_log log;
    u32 max_batch;
    usize rec_cap;

    pthread_mutex_t mu;
    u64 next_lsn;
    u64 next_seq; /* the number the next pushed message gets */
    u8 *rec; /* holds one encoded record */
    mq_reader *open;

    pthread_mutex_t sync_mu;
    pthread_cond_t synced; /* broadcast when the durable LSN moves */
    u64 committed_lsn; /* the newest committed change */
    u64 committed_seq; /* next_seq as of committed_lsn */
    u64 durable_lsn; /* changes through this LSN are durable */
    u64 durable_seq; /* messages below this are durable */
    bool flushing; /* a leader is flushing the WAL */

    bool mu_init;
    bool sync_init;
    bool cond_init;
};

void mq_config_init(mq_config *cfg)
{
    ps_config pc;
    ps_config_init(&pc);
    *cfg = (mq_config) {
        .cap = 1000000,
        .max_batch = 1u << 20,
        .checkpoint_interval_ms = pc.checkpoint_interval_ms,
    };
}

ps_store *mq_store(mq *q)
{
    return q->store;
}

const ml_log *mq_log(mq *q)
{
    return &q->log;
}

/* ---- records -------------------------------------------------------------- */

static usize rec_create(u8 *rec, u64 cap)
{
    rec[0] = REC_CREATE;
    ps_put_le64(rec + 1, cap);
    return REC_CREATE_SIZE;
}

static usize rec_push(u8 *rec, const mq_msg *msgs, u32 n)
{
    rec[0] = REC_PUSH;
    ps_put_le32(rec + 1, n);
    usize at = REC_PUSH_HEADER;
    for (u32 i = 0; i < n; i++) {
        ps_put_le32(rec + at, msgs[i].len);
        at += REC_MSG_HEADER;
        if (msgs[i].len > 0) {
            memcpy(rec + at, msgs[i].data, msgs[i].len);
        }
        at += msgs[i].len;
    }
    return at;
}

/* Encodes sub, unsub or ack. Only sub and ack carry pos. */
static usize rec_reader(u8 *rec, u8 type, const char *name, u64 pos)
{
    const usize nlen = strlen(name);
    rec[0] = type;
    rec[1] = (u8) nlen;
    memcpy(rec + REC_READER_HEADER, name, nlen);
    usize len = REC_READER_HEADER + nlen;
    if (type != REC_UNSUB) {
        ps_put_le64(rec + len, pos);
        len += 8;
    }
    return len;
}

static int apply_push(mq *q, const u8 *rec, usize len, u64 *first)
{
    if (len < REC_PUSH_HEADER) {
        return EBADMSG;
    }
    const u32 n = ps_get_le32(rec + 1);
    usize at = REC_PUSH_HEADER;
    for (u32 i = 0; i < n; i++) {
        if (len - at < REC_MSG_HEADER) {
            return EBADMSG;
        }
        const u32 mlen = ps_get_le32(rec + at);
        at += REC_MSG_HEADER;
        if (len - at < mlen) {
            return EBADMSG;
        }
        u64 seq;
        const int rv = ml_push(&q->log, rec + at, mlen, &seq);
        if (rv != 0) {
            return rv;
        }
        if (i == 0) {
            *first = seq;
        }
        q->next_seq = seq + 1;
        at += mlen;
    }
    return at == len ? 0 : EBADMSG;
}

static int apply_reader(mq *q, const u8 *rec, usize len)
{
    if (len < REC_READER_HEADER) {
        return EBADMSG;
    }
    const usize nlen = rec[1];
    if (nlen == 0 || nlen > MQ_NAME_MAX || len < REC_READER_HEADER + nlen) {
        return EBADMSG;
    }
    char name[MQ_NAME_MAX + 1];
    memcpy(name, rec + REC_READER_HEADER, nlen);
    name[nlen] = '\0';
    const u8 *rest = rec + REC_READER_HEADER + nlen;
    const usize rest_len = len - REC_READER_HEADER - nlen;
    if (rec[0] == REC_UNSUB) {
        return rest_len == 0 ? ml_reader_remove(&q->log, name) : EBADMSG;
    }
    if (rest_len != 8) {
        return EBADMSG;
    }
    if (rec[0] == REC_SUB) {
        return ml_reader_add(&q->log, name, ps_get_le64(rest));
    }
    return ml_reader_ack(&q->log, name, ps_get_le64(rest));
}

/* Applies one record inside the current operation. A push sets *first. */
static int apply(mq *q, const u8 *rec, usize len, u64 *first)
{
    if (len == 0) {
        return EBADMSG;
    }
    switch (rec[0]) {
        case REC_CREATE: {
            if (len != REC_CREATE_SIZE) {
                return EBADMSG;
            }
            ps_page_id meta;
            const int rv = ml_create(q->store, ps_get_le64(rec + 1), &meta);
            return rv == 0 && !ps_page_id_eq(meta, q->log.meta) ? EEXIST : rv;
        }
        case REC_PUSH:
            return apply_push(q, rec, len, first);
        case REC_SUB:
        case REC_UNSUB:
        case REC_ACK:
            return apply_reader(q, rec, len);
        default:
            return EBADMSG;
    }
}

/* Holds the queue that recovery replays records into. */
struct ps_apply_ctx {
    mq *q;
};

static int replay(ps_apply_ctx *ctx, ps_store *s, u64 lsn, const void *rec, usize len)
{
    int rv = ps_op_begin(s, lsn);
    if (rv != 0) {
        return rv;
    }
    u64 first;
    rv = apply(ctx->q, rec, len, &first);
    if (rv != 0) {
        ps_op_abort(s);
        return rv;
    }
    ps_op_commit(s);
    return 0;
}

/* ---- changes -------------------------------------------------------------- */

/*
 * Runs the len-byte record in q->rec as the next operation and logs it.
 * Call with mu held, then wait_durable(*lsn) after releasing it.
 */
static int run_locked(mq *q, usize len, u64 *lsn, u64 *first)
{
    const u64 l = q->next_lsn;
    const u64 seq = q->next_seq;
    int rv = ps_op_begin(q->store, l);
    if (rv != 0) {
        return rv;
    }
    rv = apply(q, q->rec, len, first);
    if (rv == 0) {
        rv = ps_wal_append(q->wal, l, q->rec, len);
    }
    if (rv != 0) {
        ps_op_abort(q->store);
        q->next_seq = seq;
        return rv;
    }
    ps_op_commit(q->store);
    q->next_lsn = l + 1;
    pthread_mutex_lock(&q->sync_mu);
    q->committed_lsn = l;
    q->committed_seq = q->next_seq;
    pthread_mutex_unlock(&q->sync_mu);
    *lsn = l;
    return 0;
}

/* Returns once lsn is durable, flushing the WAL if no other thread is. */
static int wait_durable(mq *q, u64 lsn)
{
    int rv = 0;
    pthread_mutex_lock(&q->sync_mu);
    while (q->durable_lsn < lsn && rv == 0) {
        if (q->flushing) {
            pthread_cond_wait(&q->synced, &q->sync_mu);
            continue;
        }
        /* Commits come before waiting, so this covers lsn. */
        const u64 target = q->committed_lsn;
        const u64 seq = q->committed_seq;
        q->flushing = true;
        pthread_mutex_unlock(&q->sync_mu);
        rv = ps_wal_flush(q->wal, target);
        pthread_mutex_lock(&q->sync_mu);
        q->flushing = false;
        if (rv == 0 && target > q->durable_lsn) {
            q->durable_lsn = target;
            q->durable_seq = seq;
        }
        pthread_cond_broadcast(&q->synced);
    }
    pthread_mutex_unlock(&q->sync_mu);
    return rv;
}

/* Runs a reader record and waits for it to be durable. */
static int change_reader(mq *q, u8 type, const char *name, u64 pos)
{
    pthread_mutex_lock(&q->mu);
    const usize len = rec_reader(q->rec, type, name, pos);
    u64 lsn;
    u64 first;
    const int rv = run_locked(q, len, &lsn, &first);
    pthread_mutex_unlock(&q->mu);
    return rv != 0 ? rv : wait_durable(q, lsn);
}

static u64 durable_seq(mq *q)
{
    pthread_mutex_lock(&q->sync_mu);
    const u64 seq = q->durable_seq;
    pthread_mutex_unlock(&q->sync_mu);
    return seq;
}

int mq_push_batch(mq *q, const mq_msg *msgs, u32 n, u64 *first)
{
    if (msgs == NULL || n == 0 || n > MQ_BATCH_MAX) {
        return EINVAL;
    }
    u64 bytes = 0;
    for (u32 i = 0; i < n; i++) {
        if (msgs[i].data == NULL && msgs[i].len > 0) {
            return EINVAL;
        }
        bytes += msgs[i].len;
    }
    if (bytes > q->max_batch) {
        return EMSGSIZE;
    }
    pthread_mutex_lock(&q->mu);
    const usize len = rec_push(q->rec, msgs, n);
    u64 lsn;
    u64 seq = 0;
    int rv = run_locked(q, len, &lsn, &seq);
    pthread_mutex_unlock(&q->mu);
    if (rv == 0) {
        rv = wait_durable(q, lsn);
    }
    if (rv == 0) {
        *first = seq;
    }
    return rv;
}

int mq_push(mq *q, const void *msg, u32 len, u64 *seq)
{
    const mq_msg m = {msg, len};
    return mq_push_batch(q, &m, 1, seq);
}

static bool name_ok(const char *name)
{
    if (name == NULL) {
        return false;
    }
    const usize n = strlen(name);
    return n >= 1 && n <= MQ_NAME_MAX;
}

int mq_subscribe(mq *q, const char *name, u64 pos)
{
    return name_ok(name) ? change_reader(q, REC_SUB, name, pos) : EINVAL;
}

static bool is_open(const mq *q, const char *name)
{
    for (const mq_reader *r = q->open; r != NULL; r = r->next_open) {
        if (strcmp(r->name, name) == 0) {
            return true;
        }
    }
    return false;
}

int mq_unsubscribe(mq *q, const char *name)
{
    if (!name_ok(name)) {
        return ENOENT;
    }
    pthread_mutex_lock(&q->mu);
    if (is_open(q, name)) {
        pthread_mutex_unlock(&q->mu);
        return EBUSY;
    }
    const usize len = rec_reader(q->rec, REC_UNSUB, name, 0);
    u64 lsn;
    u64 first;
    const int rv = run_locked(q, len, &lsn, &first);
    pthread_mutex_unlock(&q->mu);
    return rv != 0 ? rv : wait_durable(q, lsn);
}

/* ---- readers -------------------------------------------------------------- */

int mq_reader_open(mq *q, const char *name, mq_reader *r)
{
    memset(r, 0, sizeof(*r));
    if (!name_ok(name)) {
        return ENOENT;
    }
    pthread_mutex_lock(&q->mu);
    int rv = is_open(q, name) ? EBUSY : 0;
    u64 pos = 0;
    if (rv == 0) {
        ps_snapshot *snap;
        rv = ps_snapshot_open(q->store, &snap);
        if (rv == 0) {
            rv = ml_reader_get(&q->log, snap, name, &pos);
            ps_snapshot_close(snap);
        }
    }
    if (rv == 0) {
        r->q = q;
        strcpy(r->name, name);
        r->next = pos;
        r->acked = pos;
        r->next_open = q->open;
        q->open = r;
    }
    pthread_mutex_unlock(&q->mu);
    return rv;
}

static void drop_snapshot(mq_reader *r)
{
    if (r->snap != NULL) {
        ps_snapshot_close(r->snap);
        r->snap = NULL;
    }
    r->limit = 0;
}

void mq_reader_close(mq_reader *r)
{
    mq *q = r->q;
    if (q == NULL) {
        return;
    }
    drop_snapshot(r);
    pthread_mutex_lock(&q->mu);
    mq_reader **p = &q->open;
    while (*p != r) {
        p = &(*p)->next_open;
    }
    *p = r->next_open;
    pthread_mutex_unlock(&q->mu);
    r->q = NULL;
}

static struct timespec deadline_after(i64 ms)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_sec += (time_t) (ms / 1000);
    t.tv_nsec += (long) (ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) {
        t.tv_sec++;
        t.tv_nsec -= 1000000000L;
    }
    return t;
}

/* Waits until message seq is durable and returns the watermark in *out. */
static int wait_message(mq *q, u64 seq, i64 timeout_ms, const struct timespec *deadline, u64 *out)
{
    int rv = 0;
    pthread_mutex_lock(&q->sync_mu);
    while (q->durable_seq <= seq && rv == 0) {
        if (timeout_ms == 0) {
            rv = ETIMEDOUT;
        } else if (timeout_ms < 0) {
            pthread_cond_wait(&q->synced, &q->sync_mu);
        } else {
            rv = pthread_cond_timedwait(&q->synced, &q->sync_mu, deadline);
        }
    }
    if (q->durable_seq > seq) {
        rv = 0;
    }
    *out = q->durable_seq;
    pthread_mutex_unlock(&q->sync_mu);
    return rv;
}

/* Opens a snapshot holding r->next once that message is durable. */
static int refresh(mq_reader *r, i64 timeout_ms, const struct timespec *deadline)
{
    mq *q = r->q;
    u64 durable;
    int rv = wait_message(q, r->next, timeout_ms, deadline, &durable);
    if (rv == 0) {
        rv = ps_snapshot_open(q->store, &r->snap);
        if (rv != 0) {
            r->snap = NULL;
        }
    }
    ml_state st;
    if (rv == 0) {
        rv = ml_state_get(&q->log, r->snap, &st);
    }
    if (rv != 0) {
        return rv;
    }
    if (r->next < st.first_seq) {
        r->missed += st.first_seq - r->next;
        r->next = st.first_seq;
    }
    r->limit = durable < st.next_seq ? durable : st.next_seq;
    return 0;
}

int mq_read(mq_reader *r, void *buf, u32 cap, i64 timeout_ms, mq_item *out)
{
    const struct timespec deadline = deadline_after(timeout_ms > 0 ? timeout_ms : 0);
    for (;;) {
        if (r->snap != NULL && r->next < r->limit) {
            u32 len;
            const int rv = ml_read(&r->q->log, r->snap, r->next, buf, cap, &len);
            if (rv == ESTALE) {
                drop_snapshot(r); /* cut off to free memory, so start over */
                continue;
            }
            if (rv != 0) {
                return rv;
            }
            out->seq = r->next;
            out->len = len;
            out->missed = r->missed;
            if (len > cap) {
                return EMSGSIZE;
            }
            r->missed = 0;
            r->next++;
            return 0;
        }
        drop_snapshot(r);
        const int rv = refresh(r, timeout_ms, &deadline);
        if (rv == ESTALE) {
            drop_snapshot(r);
        } else if (rv != 0) {
            return rv;
        }
    }
}

int mq_ack(mq_reader *r, u64 seq)
{
    mq *q = r->q;
    if (seq >= durable_seq(q)) {
        return EINVAL;
    }
    const u64 pos = seq + 1;
    if (pos <= r->acked) {
        return 0;
    }
    const int rv = change_reader(q, REC_ACK, r->name, pos);
    if (rv != 0) {
        return rv;
    }
    r->acked = pos;
    if (r->next < pos) {
        r->next = pos;
    }
    return 0;
}

/* ---- open and close ------------------------------------------------------- */

static int init_locks(mq *q)
{
    int rv = pthread_mutex_init(&q->mu, NULL);
    q->mu_init = rv == 0;
    if (rv == 0) {
        rv = pthread_mutex_init(&q->sync_mu, NULL);
        q->sync_init = rv == 0;
    }
    pthread_condattr_t attr;
    if (rv == 0) {
        rv = pthread_condattr_init(&attr);
    }
    if (rv == 0) {
        rv = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        if (rv == 0) {
            rv = pthread_cond_init(&q->synced, &attr);
            q->cond_init = rv == 0;
        }
        pthread_condattr_destroy(&attr);
    }
    return rv;
}

void mq_close(mq *q)
{
    if (q == NULL) {
        return;
    }
    /* The store's checkpoint hooks use the WAL, so close the store first. */
    ps_close(q->store);
    ps_wal_close(q->wal);
    free(q->rec);
    if (q->cond_init) {
        pthread_cond_destroy(&q->synced);
    }
    if (q->sync_init) {
        pthread_mutex_destroy(&q->sync_mu);
    }
    if (q->mu_init) {
        pthread_mutex_destroy(&q->mu);
    }
    free(q);
}

int mq_checkpoint(mq *q)
{
    return ps_checkpoint(q->store);
}

static int open_store(mq *q, const char *dir, const mq_config *cfg, u32 flags)
{
    ps_config pc;
    ps_config_init(&pc);
    pc.page_size = cfg->page_size;
    if (cfg->cache_size != 0) {
        pc.cache_size = cfg->cache_size;
    }
    pc.checkpoint_interval_ms = cfg->checkpoint_interval_ms;
    pc.vfs = cfg->vfs;
    pc.log = cfg->log;
    pc.log_ctx = cfg->log_ctx;
    int rv = ps_open(dir, &pc, flags, &q->store);
    if (rv != 0) {
        q->store = NULL;
        return rv;
    }
    ps_wal_config wc;
    ps_wal_config_init(&wc);
    wc.max_record_size = (u32) q->rec_cap;
    wc.segment_size = WAL_MIN_SEGMENT;
    while (wc.segment_size < 2 * (u64) q->rec_cap + ((u64) 64 << 10)) {
        wc.segment_size *= 2;
    }
    rv = ps_wal_open(q->store, NULL, &wc, &q->wal);
    if (rv != 0) {
        q->wal = NULL;
    }
    return rv;
}

/* Replays the WAL and creates the log in a new store. */
static int recover(mq *q, u64 cap)
{
    ml_open(q->store, MQ_META_ID, &q->log);
    struct ps_apply_ctx actx = {q};
    int rv = ps_recover(q->store, q->wal, replay, &actx);
    if (rv != 0) {
        return rv;
    }
    q->next_lsn = ps_committed_lsn(q->store) + 1;
    if (ps_committed_lsn(q->store) == 0) {
        u64 lsn;
        u64 first;
        rv = run_locked(q, rec_create(q->rec, cap), &lsn, &first);
        if (rv == 0) {
            rv = wait_durable(q, lsn);
        }
        if (rv != 0) {
            return rv;
        }
    }
    /* Recovery leaves only durable records, so everything kept is durable. */
    ps_snapshot *snap;
    rv = ps_snapshot_open(q->store, &snap);
    if (rv != 0) {
        return rv;
    }
    ml_state st;
    rv = ml_state_get(&q->log, snap, &st);
    ps_snapshot_close(snap);
    q->next_seq = st.next_seq;
    q->committed_lsn = q->next_lsn - 1;
    q->committed_seq = st.next_seq;
    q->durable_lsn = q->next_lsn - 1;
    q->durable_seq = st.next_seq;
    return rv;
}

int mq_open(const char *dir, const mq_config *cfg, u32 flags, mq **out)
{
    *out = NULL;
    if (cfg->cap == 0 || cfg->max_batch == 0 || cfg->max_batch > MQ_MAX_BATCH_LIMIT) {
        return EINVAL;
    }
    mq *q = calloc(1, sizeof(*q));
    if (q == NULL) {
        return ENOMEM;
    }
    q->max_batch = cfg->max_batch;
    q->rec_cap = REC_PUSH_HEADER + (usize) MQ_BATCH_MAX * REC_MSG_HEADER + cfg->max_batch;
    q->rec = malloc(q->rec_cap);
    int rv = q->rec == NULL ? ENOMEM : init_locks(q);
    if (rv == 0) {
        rv = open_store(q, dir, cfg, flags);
    }
    if (rv == 0) {
        rv = recover(q, cfg->cap);
    }
    if (rv != 0) {
        mq_close(q);
        return rv;
    }
    *out = q;
    return 0;
}
