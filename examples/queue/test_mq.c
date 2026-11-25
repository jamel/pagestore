/*
 * Tests for mq: the API on a real directory, producer and reader threads
 * at full speed, and power loss through the fault VFS.
 *
 * After each crash, every successful push must survive with its bytes,
 * every reader's position must be at least its last successful ack, and no
 * reader may have received a message the crash took back.
 */

#include "fault_vfs.h"
#include "mq.h"
#include "msglog.h"
#include "rng.h"
#include "test.h"

#include "pagestore/bytes.h"
#include "pagestore/pagestore.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void log_line(ps_logger_ctx *ctx, ps_log_level level, const char *fmt, va_list ap)
{
    PS_UNUSED(ctx, level);
    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    fprintf(stderr, "store: %s\n", msg);
}

static mq *open_queue(const char *dir, u64 cap)
{
    mq_config cfg;
    mq_config_init(&cfg);
    cfg.cap = cap;
    cfg.cache_size = (usize) 4 << 20;
    cfg.logger = (ps_logger) {.max_level = PS_LOG_LEVEL_WARN, .write = log_line};
    mq *q;
    T_OK(mq_open(dir, &cfg, PS_CREATE, &q));
    return q;
}

/* Runs the log check and looks for leaked pages. */
static ml_state check_queue(mq *q)
{
    ps_snapshot *snap;
    T_OK(ps_snapshot_open(mq_store(q), &snap));
    ml_state st;
    T_OK(ml_state_get(mq_log(q), snap, &st));
    ml_stats ms;
    char msg[256] = "";
    const int rv = ml_check(mq_log(q), snap, &ms, msg, sizeof(msg));
    ps_snapshot_close(snap);
    if (rv != 0) {
        T_FAIL("ml_check: %s (%d)", msg, rv);
    }
    struct ps_stats ss;
    ps_stats(mq_store(q), &ss);
    T_EQ(ss.allocated_pages, 1 + ms.data_pages + ms.index_pages + ms.map_pages);
    return st;
}

static u64 reader_pos(mq *q, const char *name)
{
    ps_snapshot *snap;
    T_OK(ps_snapshot_open(mq_store(q), &snap));
    u64 pos;
    T_OK(ml_reader_get(mq_log(q), snap, name, &pos));
    ps_snapshot_close(snap);
    return pos;
}

static void sleep_us(u32 us)
{
    const struct timespec t = {us / 1000000, (long) (us % 1000000) * 1000L};
    nanosleep(&t, NULL);
}

static void sleep_ms(u32 ms)
{
    sleep_us(ms * 1000);
}

/* ---- test messages -------------------------------------------------------- */

/*
 * A test message starts with its producer and that producer's counter.
 * Its length and the rest of its bytes are derived from both.
 */
#define MSG_HEADER 12u

static u64 mix(u64 x)
{
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDull;
    x ^= x >> 33;
    return x;
}

static u32 msg_size(u32 producer, u64 counter, u32 max_extra)
{
    return MSG_HEADER + (u32) (mix(((u64) producer << 40) ^ counter) % (max_extra + 1));
}

static u8 msg_byte(u32 producer, u64 counter, u32 i)
{
    return (u8) (producer * 131u + counter * 31u + i);
}

static u32 msg_fill(u8 *buf, u32 producer, u64 counter, u32 max_extra)
{
    const u32 size = msg_size(producer, counter, max_extra);
    ps_put_le32(buf, producer);
    ps_put_le64(buf + 4, counter);
    for (u32 i = MSG_HEADER; i < size; i++) {
        buf[i] = msg_byte(producer, counter, i);
    }
    return size;
}

/* Returns false unless buf holds a message msg_fill made. */
static bool msg_parse(
    const u8 *buf,
    u32 len,
    u32 max_extra,
    u32 producers,
    u32 *producer,
    u64 *counter)
{
    if (len < MSG_HEADER) {
        return false;
    }
    memcpy(producer, buf, 4);
    memcpy(counter, buf + 4, 8);
    if (*producer >= producers || len != msg_size(*producer, *counter, max_extra)) {
        return false;
    }
    for (u32 i = MSG_HEADER; i < len; i++) {
        if (buf[i] != msg_byte(*producer, *counter, i)) {
            return false;
        }
    }
    return true;
}

/* ---- API ------------------------------------------------------------------ */

static void expect_read(mq_reader *r, u64 seq, const char *want)
{
    char buf[64];
    mq_item it;
    T_OK(mq_read(r, buf, sizeof(buf), 0, &it));
    T_EQ(it.seq, seq);
    T_EQ(it.len, strlen(want));
    T_MEMEQ(buf, want, it.len);
}

static void test_mq_basic(void)
{
    char *dir = t_tmpdir();
    mq *q = open_queue(dir, 1000);
    T_OK(mq_subscribe(q, "a", MQ_OLDEST));
    T_ERR(mq_subscribe(q, "a", MQ_OLDEST), EEXIST);
    T_ERR(mq_subscribe(q, "", MQ_OLDEST), EINVAL);
    T_ERR(mq_subscribe(q, "a-name-longer-than-23-bb", MQ_OLDEST), EINVAL);

    u64 seq;
    T_OK(mq_push(q, "one", 3, &seq));
    T_EQ(seq, 0);
    const mq_msg two[] = {{"two", 3}, {"three", 5}};
    T_OK(mq_push_batch(q, two, 2, &seq));
    T_EQ(seq, 1);
    T_ERR(mq_push_batch(q, two, 0, &seq), EINVAL);
    T_OK(mq_subscribe(q, "late", MQ_LATEST));

    mq_reader r;
    mq_reader r2;
    T_OK(mq_reader_open(q, "a", &r));
    T_ERR(mq_reader_open(q, "a", &r2), EBUSY);
    T_ERR(mq_reader_open(q, "nobody", &r2), ENOENT);
    T_ERR(mq_unsubscribe(q, "a"), EBUSY);

    /* A short buffer gets the length, and the message comes again. */
    char buf[16];
    mq_item it;
    T_ERR(mq_read(&r, buf, 2, 0, &it), EMSGSIZE);
    T_EQ(it.seq, 0);
    T_EQ(it.len, 3);
    expect_read(&r, 0, "one");
    expect_read(&r, 1, "two");
    expect_read(&r, 2, "three");
    T_ERR(mq_read(&r, buf, sizeof(buf), 0, &it), ETIMEDOUT);
    T_ERR(mq_read(&r, buf, sizeof(buf), 20, &it), ETIMEDOUT);
    T_ERR(mq_ack(&r, 3), EINVAL); /* not pushed yet */
    T_OK(mq_ack(&r, 1));
    T_OK(mq_ack(&r, 0)); /* already covered */
    T_EQ(reader_pos(q, "a"), 2);
    mq_reader_close(&r);

    /* Delivery is at least once: the unacknowledged message comes again. */
    T_OK(mq_reader_open(q, "a", &r));
    expect_read(&r, 2, "three");
    mq_reader_close(&r);

    /* A reader added at the latest message sees only newer ones. */
    T_OK(mq_push(q, "four", 4, &seq));
    T_EQ(seq, 3);
    T_OK(mq_push(q, "five", 4, &seq));
    T_OK(mq_reader_open(q, "late", &r));
    expect_read(&r, 3, "four");
    mq_reader_close(&r);

    /* Acknowledging ahead skips the messages in between. */
    T_OK(mq_subscribe(q, "skip", MQ_OLDEST));
    T_OK(mq_reader_open(q, "skip", &r));
    T_OK(mq_ack(&r, 2));
    expect_read(&r, 3, "four");
    mq_reader_close(&r);

    /* Reopening without a checkpoint replays the WAL. */
    mq_close(q);
    q = open_queue(dir, 1000);
    T_OK(mq_reader_open(q, "a", &r));
    expect_read(&r, 2, "three");
    expect_read(&r, 3, "four");
    expect_read(&r, 4, "five");
    mq_reader_close(&r);
    T_EQ(reader_pos(q, "late"), 3);
    T_OK(mq_unsubscribe(q, "late"));
    T_ERR(mq_unsubscribe(q, "late"), ENOENT);
    const ml_state st = check_queue(q);
    T_EQ(st.next_seq, 5);
    T_EQ(st.readers, 2);
    T_OK(mq_checkpoint(q));
    mq_close(q);
    t_rmtree(dir);
    free(dir);
}

/* A reader that falls more than the cap behind hears how many it missed. */
static void test_mq_missed(void)
{
    char *dir = t_tmpdir();
    mq *q = open_queue(dir, 10);
    T_OK(mq_subscribe(q, "slow", MQ_OLDEST));
    for (u32 i = 0; i < 25; i++) {
        char m[8];
        const int n = snprintf(m, sizeof(m), "m%u", i);
        u64 seq;
        T_OK(mq_push(q, m, (u32) n, &seq));
    }
    mq_reader r;
    T_OK(mq_reader_open(q, "slow", &r));
    char buf[8];
    mq_item it;
    T_OK(mq_read(&r, buf, sizeof(buf), 0, &it));
    T_EQ(it.seq, 15);
    T_EQ(it.missed, 15);
    T_MEMEQ(buf, "m15", 3);
    T_OK(mq_read(&r, buf, sizeof(buf), 0, &it));
    T_EQ(it.seq, 16);
    T_EQ(it.missed, 0);
    T_OK(mq_ack(&r, 16));
    mq_reader_close(&r);
    T_EQ(reader_pos(q, "slow"), 17);
    const ml_state st = check_queue(q);
    T_EQ(st.first_seq, 15);
    mq_close(q);
    t_rmtree(dir);
    free(dir);
}

typedef struct waiter {
    mq *q;
    pthread_t thread;
    mq_item it;
    int rv;
} waiter;

static void *wait_main(void *arg)
{
    waiter *w = arg;
    mq_reader r;
    w->rv = mq_reader_open(w->q, "w", &r);
    if (w->rv == 0) {
        char buf[8];
        w->rv = mq_read(&r, buf, sizeof(buf), -1, &w->it);
        mq_reader_close(&r);
    }
    return NULL;
}

/* A reader waiting with no timeout wakes when a message arrives. */
static void test_mq_wakeup(void)
{
    char *dir = t_tmpdir();
    mq *q = open_queue(dir, 100);
    T_OK(mq_subscribe(q, "w", MQ_LATEST));
    waiter w = {.q = q};
    T_OK(pthread_create(&w.thread, NULL, wait_main, &w));
    sleep_ms(50);
    u64 seq;
    T_OK(mq_push(q, "hi", 2, &seq));
    T_OK(pthread_join(w.thread, NULL));
    T_OK(w.rv);
    T_EQ(w.it.seq, seq);
    mq_close(q);
    t_rmtree(dir);
    free(dir);
}

/*
 * With a tiny cache, pushes cut off the snapshot a reader keeps between
 * reads, and the reader must start a new one without losing its place.
 */
static void test_mq_cut_off(void)
{
    char *dir = t_tmpdir();
    mq_config cfg;
    mq_config_init(&cfg);
    cfg.cache_size = (usize) 24 * 4096;
    cfg.max_batch = 4096;
    cfg.logger = (ps_logger) {.max_level = PS_LOG_LEVEL_WARN, .write = log_line};
    mq *q;
    T_OK(mq_open(dir, &cfg, PS_CREATE, &q));
    T_OK(mq_subscribe(q, "r", MQ_OLDEST));
    u8 buf[MSG_HEADER + 1000];
    u64 seq;
    const u64 total = 3000;
    for (u64 i = 0; i < 20; i++) {
        T_OK(mq_push(q, buf, msg_fill(buf, 0, i, 1000), &seq));
    }
    mq_reader r;
    T_OK(mq_reader_open(q, "r", &r));
    mq_item it;
    T_OK(mq_read(&r, buf, sizeof(buf), 0, &it));
    T_EQ(it.seq, 0);
    for (u64 i = 20; i < total; i++) {
        T_OK(mq_push(q, buf, msg_fill(buf, 0, i, 1000), &seq));
    }
    for (u64 i = 1; i < total; i++) {
        T_OK(mq_read(&r, buf, sizeof(buf), 0, &it));
        T_EQ(it.seq, i);
        u32 producer;
        u64 counter;
        T_CHECK(msg_parse(buf, it.len, 1000, 1, &producer, &counter));
        T_EQ(counter, i);
    }
    mq_reader_close(&r);
    struct ps_stats ss;
    ps_stats(mq_store(q), &ss);
    T_CHECK(ss.snapshots_cut_off > 0);
    mq_close(q);
    t_rmtree(dir);
    free(dir);
}

/* ---- threads -------------------------------------------------------------- */

#define TH_PRODUCERS 4u
#define TH_READERS 3u
#define TH_EXTRA 300u

typedef struct th_state {
    mq *q;
    u64 per_producer;
    u64 total;
} th_state;

typedef struct th_producer {
    th_state *st;
    pthread_t thread;
    u32 id;
} th_producer;

typedef struct th_reader {
    th_state *st;
    pthread_t thread;
    u32 id;
    u64 delivered;
} th_reader;

static void *th_produce(void *arg)
{
    th_producer *p = arg;
    rng r;
    rng_seed(&r, 100 + p->id);
    u8 *buf = malloc(8 * (MSG_HEADER + TH_EXTRA));
    T_CHECK(buf != NULL);
    u64 counter = 0;
    while (counter < p->st->per_producer) {
        u32 n = 1 + (u32) rng_below(&r, 8);
        if (n > p->st->per_producer - counter) {
            n = (u32) (p->st->per_producer - counter);
        }
        mq_msg msgs[8];
        u8 *at = buf;
        for (u32 i = 0; i < n; i++) {
            const u32 len = msg_fill(at, p->id, counter + i, TH_EXTRA);
            msgs[i] = (mq_msg) {at, len};
            at += len;
        }
        u64 first;
        T_OK(mq_push_batch(p->st->q, msgs, n, &first));
        counter += n;
    }
    free(buf);
    return NULL;
}

/*
 * Each reader must see every producer's messages exactly once and in
 * order. Reader 0 also reopens its handle right after some acks.
 */
static void *th_read(void *arg)
{
    th_reader *r = arg;
    char name[8];
    snprintf(name, sizeof(name), "t%u", r->id);
    mq_reader h;
    T_OK(mq_reader_open(r->st->q, name, &h));
    u64 next[TH_PRODUCERS] = {0};
    u8 buf[MSG_HEADER + TH_EXTRA];
    while (r->delivered < r->st->total) {
        mq_item it;
        T_OK(mq_read(&h, buf, sizeof(buf), 10000, &it));
        T_EQ(it.missed, 0);
        u32 producer;
        u64 counter;
        T_CHECK(msg_parse(buf, it.len, TH_EXTRA, TH_PRODUCERS, &producer, &counter));
        T_EQ(counter, next[producer]);
        next[producer]++;
        r->delivered++;
        if (r->delivered % 100 == 0 || r->delivered == r->st->total) {
            T_OK(mq_ack(&h, it.seq));
            if (r->id == 0 && r->delivered % 500 == 0) {
                mq_reader_close(&h);
                T_OK(mq_reader_open(r->st->q, name, &h));
            }
        }
    }
    mq_reader_close(&h);
    return NULL;
}

static void test_mq_threads(void)
{
    char *dir = t_tmpdir();
    th_state st = {.per_producer = 2000 * t_scale()};
    st.total = st.per_producer * TH_PRODUCERS;
    st.q = open_queue(dir, st.total);
    for (u32 i = 0; i < TH_READERS; i++) {
        char name[8];
        snprintf(name, sizeof(name), "t%u", i);
        T_OK(mq_subscribe(st.q, name, MQ_LATEST));
    }
    th_reader rs[TH_READERS];
    th_producer ps[TH_PRODUCERS];
    for (u32 i = 0; i < TH_READERS; i++) {
        rs[i] = (th_reader) {.st = &st, .id = i};
        T_OK(pthread_create(&rs[i].thread, NULL, th_read, &rs[i]));
    }
    for (u32 i = 0; i < TH_PRODUCERS; i++) {
        ps[i] = (th_producer) {.st = &st, .id = i};
        T_OK(pthread_create(&ps[i].thread, NULL, th_produce, &ps[i]));
    }
    for (u32 i = 0; i < TH_PRODUCERS; i++) {
        T_OK(pthread_join(ps[i].thread, NULL));
    }
    for (u32 i = 0; i < TH_READERS; i++) {
        T_OK(pthread_join(rs[i].thread, NULL));
        T_EQ(rs[i].delivered, st.total);
    }
    const ml_state ls = check_queue(st.q);
    T_EQ(ls.next_seq, st.total);
    for (u32 i = 0; i < TH_READERS; i++) {
        char name[8];
        snprintf(name, sizeof(name), "t%u", i);
        T_EQ(reader_pos(st.q, name), st.total);
    }
    mq_close(st.q);
    t_rmtree(dir);
    free(dir);
}

/* ---- power loss ----------------------------------------------------------- */

#define CR_DIR "/q"
#define CR_PRODUCERS 3u
#define CR_READERS 2u
#define CR_CAP 3000u
#define CR_EXTRA 3000u
#define CR_BATCH 4u

typedef struct pushed {
    u64 seq;
    u32 producer;
    u64 counter;
} pushed;

typedef struct crash_state {
    fault_vfs *fv;
    mq *q;
    atomic_bool stop;
    u64 counters[CR_PRODUCERS]; /* next counter for each producer, across rounds */
    u64 seed;

    pthread_mutex_t mu; /* guards the oracle below */
    pushed *ok; /* successful pushes that may still be kept */
    usize nok;
    usize ok_cap;
    u64 acked[CR_READERS]; /* highest position acknowledged successfully */
    u64 seen; /* one past the highest message any reader received */
} crash_state;

typedef struct crash_thread {
    crash_state *c;
    pthread_t thread;
    u32 id;
} crash_thread;

static void record_push(crash_state *c, u64 first, u32 producer, u64 counter, u32 n)
{
    pthread_mutex_lock(&c->mu);
    if (c->nok + n > c->ok_cap) {
        c->ok_cap = c->ok_cap == 0 ? 4096 : 2 * c->ok_cap;
        c->ok = realloc(c->ok, c->ok_cap * sizeof(*c->ok));
        T_CHECK(c->ok != NULL);
    }
    for (u32 i = 0; i < n; i++) {
        c->ok[c->nok++] = (pushed) {first + i, producer, counter + i};
    }
    pthread_mutex_unlock(&c->mu);
}

static void *crash_produce(void *arg)
{
    crash_thread *t = arg;
    crash_state *c = t->c;
    rng r;
    rng_seed(&r, c->seed * 31 + t->id);
    u8 *buf = malloc(CR_BATCH * (MSG_HEADER + CR_EXTRA));
    T_CHECK(buf != NULL);
    u64 counter = c->counters[t->id];
    while (!atomic_load(&c->stop)) {
        const u32 n = 1 + (u32) rng_below(&r, CR_BATCH);
        mq_msg msgs[CR_BATCH];
        u8 *at = buf;
        for (u32 i = 0; i < n; i++) {
            const u32 len = msg_fill(at, t->id, counter + i, CR_EXTRA);
            msgs[i] = (mq_msg) {at, len};
            at += len;
        }
        u64 first;
        const int rv = mq_push_batch(c->q, msgs, n, &first);
        if (rv == 0) {
            record_push(c, first, t->id, counter, n);
        }
        /* A failed push may still survive, so its counters stay used. */
        counter += n;
        if (rv != 0) {
            break;
        }
        /* Pause so reader 0 keeps up and reads right behind the newest push. */
        sleep_us((u32) rng_below(&r, 400));
    }
    c->counters[t->id] = counter;
    free(buf);
    return NULL;
}

static void record_ack(crash_state *c, u32 reader, u64 pos)
{
    pthread_mutex_lock(&c->mu);
    if (pos > c->acked[reader]) {
        c->acked[reader] = pos;
    }
    pthread_mutex_unlock(&c->mu);
}

/*
 * Starts reader 0 at the newest message, right behind the producers, where
 * a message delivered before it is durable would show up. Everything kept
 * at open is durable, so acknowledging up to it is allowed.
 */
static bool skip_to_newest(crash_state *c, mq_reader *h)
{
    ps_snapshot *snap;
    if (ps_snapshot_open(mq_store(c->q), &snap) != 0) {
        return false;
    }
    ml_state st;
    const int rv = ml_state_get(mq_log(c->q), snap, &st);
    ps_snapshot_close(snap);
    if (rv != 0 || st.next_seq == 0) {
        return rv == 0;
    }
    if (mq_ack(h, st.next_seq - 1) != 0) {
        return false;
    }
    record_ack(c, 0, st.next_seq);
    return true;
}

static void *crash_read(void *arg)
{
    crash_thread *t = arg;
    crash_state *c = t->c;
    rng r;
    rng_seed(&r, c->seed * 37 + t->id);
    char name[8];
    snprintf(name, sizeof(name), "r%u", t->id);
    mq_reader h;
    if (mq_reader_open(c->q, name, &h) != 0) {
        return NULL;
    }
    if (t->id == 0 && !skip_to_newest(c, &h)) {
        mq_reader_close(&h);
        return NULL;
    }
    u8 *buf = malloc(MSG_HEADER + CR_EXTRA);
    T_CHECK(buf != NULL);
    u64 last[CR_PRODUCERS];
    memset(last, 0xFF, sizeof(last));
    while (!atomic_load(&c->stop)) {
        mq_item it;
        int rv = mq_read(&h, buf, MSG_HEADER + CR_EXTRA, 20, &it);
        if (rv == ETIMEDOUT) {
            continue;
        }
        if (rv != 0) {
            break;
        }
        u32 producer;
        u64 counter;
        T_CHECK(msg_parse(buf, it.len, CR_EXTRA, CR_PRODUCERS, &producer, &counter));
        T_CHECK(last[producer] == UINT64_MAX || counter > last[producer]);
        last[producer] = counter;
        pthread_mutex_lock(&c->mu);
        if (it.seq + 1 > c->seen) {
            c->seen = it.seq + 1;
        }
        pthread_mutex_unlock(&c->mu);
        if (rng_below(&r, 4) == 0) {
            rv = mq_ack(&h, it.seq);
            if (rv != 0) {
                break;
            }
            record_ack(c, t->id, it.seq + 1);
        }
    }
    mq_reader_close(&h);
    free(buf);
    return NULL;
}

static void crash_open(crash_state *c)
{
    mq_config cfg;
    mq_config_init(&cfg);
    cfg.cap = CR_CAP;
    cfg.max_batch = 64u << 10;
    cfg.cache_size = (usize) 512 * 4096;
    cfg.checkpoint_interval_ms = 5;
    cfg.vfs = fault_vfs_get(c->fv);
    T_OK(mq_open(CR_DIR, &cfg, PS_CREATE, &c->q));
}

/* Checks the reopened queue against the oracle, then forgets trimmed pushes. */
static void crash_verify(crash_state *c)
{
    const ml_state st = check_queue(c->q);
    if (c->seen > st.next_seq) {
        T_FAIL(
            "a reader got message %llu, but only %llu survived",
            (unsigned long long) c->seen - 1,
            (unsigned long long) st.next_seq);
    }
    ps_snapshot *snap;
    T_OK(ps_snapshot_open(mq_store(c->q), &snap));
    const ml_log *l = mq_log(c->q);
    u8 *buf = malloc(MSG_HEADER + CR_EXTRA);
    T_CHECK(buf != NULL);
    usize kept = 0;
    for (usize i = 0; i < c->nok; i++) {
        const pushed *p = &c->ok[i];
        if (p->seq >= st.next_seq) {
            T_FAIL("acknowledged push %llu was lost", (unsigned long long) p->seq);
        }
        if (p->seq < st.first_seq) {
            continue;
        }
        u32 len;
        u32 producer;
        u64 counter;
        T_OK(ml_read(l, snap, p->seq, buf, MSG_HEADER + CR_EXTRA, &len));
        T_CHECK(msg_parse(buf, len, CR_EXTRA, CR_PRODUCERS, &producer, &counter));
        T_EQ(producer, p->producer);
        T_EQ(counter, p->counter);
        c->ok[kept++] = *p;
    }
    c->nok = kept;
    /* Every kept message is well formed, and each producer's are in order. */
    u64 last[CR_PRODUCERS];
    memset(last, 0xFF, sizeof(last));
    for (u64 seq = st.first_seq; seq < st.next_seq; seq++) {
        u32 len;
        u32 producer;
        u64 counter;
        T_OK(ml_read(l, snap, seq, buf, MSG_HEADER + CR_EXTRA, &len));
        T_CHECK(msg_parse(buf, len, CR_EXTRA, CR_PRODUCERS, &producer, &counter));
        T_CHECK(last[producer] == UINT64_MAX || counter > last[producer]);
        last[producer] = counter;
    }
    for (u32 i = 0; i < CR_READERS; i++) {
        char name[8];
        snprintf(name, sizeof(name), "r%u", i);
        u64 pos;
        T_OK(ml_reader_get(l, snap, name, &pos));
        T_CHECK(pos >= c->acked[i]);
    }
    ps_snapshot_close(snap);
    free(buf);
}

/* Runs the threads until the armed crash happens, then stops them. */
static void crash_round(crash_state *c, rng *r)
{
    atomic_store(&c->stop, false);
    fault_vfs_crash_at(c->fv, fault_vfs_points(c->fv) + 1 + rng_below(r, 3000));
    crash_thread ts[CR_PRODUCERS + CR_READERS];
    for (u32 i = 0; i < CR_PRODUCERS + CR_READERS; i++) {
        const bool producer = i < CR_PRODUCERS;
        ts[i] = (crash_thread) {.c = c, .id = producer ? i : i - CR_PRODUCERS};
        T_OK(pthread_create(&ts[i].thread, NULL, producer ? crash_produce : crash_read, &ts[i]));
    }
    const u64 start = t_now_ms();
    while (!fault_vfs_crashed(c->fv)) {
        if (t_now_ms() - start > 3000) {
            fault_vfs_crash_at(c->fv, fault_vfs_points(c->fv) + 1);
        }
        sleep_ms(1);
    }
    atomic_store(&c->stop, true);
    for (u32 i = 0; i < CR_PRODUCERS + CR_READERS; i++) {
        T_OK(pthread_join(ts[i].thread, NULL));
    }
    mq_close(c->q);
    c->q = NULL;
}

static void test_mq_crash(void)
{
    crash_state c = {.seed = 7};
    T_OK(pthread_mutex_init(&c.mu, NULL));
    c.fv = fault_vfs_create(c.seed);
    rng r;
    rng_seed(&r, c.seed);
    crash_open(&c);
    for (u32 i = 0; i < CR_READERS; i++) {
        char name[8];
        snprintf(name, sizeof(name), "r%u", i);
        T_OK(mq_subscribe(c.q, name, MQ_OLDEST));
    }
    const u32 rounds = 40 * t_scale();
    u64 survived = 0;
    for (u32 round = 0; round < rounds; round++) {
        c.seed = round;
        crash_round(&c, &r);
        fault_vfs_reboot(c.fv, round % 2 == 0 ? FAULT_DROP_UNSYNCED : FAULT_RANDOM_UNSYNCED);
        crash_open(&c);
        crash_verify(&c);
        survived += c.nok;
    }
    /* Make sure the rounds did real work. */
    T_CHECK(survived > 0);
    T_OK(mq_checkpoint(c.q));
    mq_close(c.q);
    free(c.ok);
    fault_vfs_destroy(c.fv);
    pthread_mutex_destroy(&c.mu);
}

int main(int argc, char **argv)
{
    t_init(argc, argv);
    T_RUN(test_mq_basic);
    T_RUN(test_mq_missed);
    T_RUN(test_mq_wakeup);
    T_RUN(test_mq_cut_off);
    T_RUN(test_mq_threads);
    T_RUN(test_mq_crash);
    return t_done();
}
