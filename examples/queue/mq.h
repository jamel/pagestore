#pragma once

/*
 * mq is a durable message queue shared by threads in one process. Producers
 * append messages to a message log (msglog.h). Each named reader reads every
 * message in order at its own pace and acknowledges what it has handled.
 * The queue keeps the newest cap messages. A reader that falls further
 * behind misses the older ones, and its next read says how many.
 *
 * A call that changes the queue returns once its change is durable, and
 * calls running at the same time share one WAL flush. Reads take no lock
 * and see only messages whose push is durable, so a reader never sees a
 * message that a crash could take back.
 *
 * Delivery is at least once. A reader that reopens resumes at its last
 * acknowledged position, so messages read but not acknowledged come again.
 *
 * Example: add a reader once, then read and acknowledge. -1 waits as long
 * as it takes.
 *
 *     int rv = mq_subscribe(q, "billing", MQ_OLDEST);
 *
 *     mq_reader r;
 *     rv = mq_reader_open(q, "billing", &r);
 *     if (rv != 0) {
 *         return rv;
 *     }
 *     for (;;) {
 *         mq_item it;
 *         rv = mq_read(&r, buf, sizeof(buf), -1, &it);
 *         if (rv != 0) {
 *             break;
 *         }
 *         handle(buf, it.len);
 *         rv = mq_ack(&r, it.seq);
 *         if (rv != 0) {
 *             break;
 *         }
 *     }
 *     mq_reader_close(&r);
 */

#include "msglog.h"

#include "pagestore/pagestore.h"

PS_EXTERN_C_BEGIN

typedef struct mq mq;

/* Start positions for mq_subscribe. Any number in between works too. */
#define MQ_OLDEST 0u
#define MQ_LATEST UINT64_MAX

#define MQ_NAME_MAX ML_NAME_MAX

/* Most messages in one mq_push_batch call. */
#define MQ_BATCH_MAX 4096u

/* Largest max_batch. */
#define MQ_MAX_BATCH_LIMIT (256u << 20)

typedef struct mq_config {
    u64 cap; /* messages kept, set when the queue is created */
    u32 max_batch; /* most payload bytes in one push or batch, up to MQ_MAX_BATCH_LIMIT */
    u32 page_size; /* 0 uses the store default */
    usize cache_size; /* 0 uses the store default */
    u32 checkpoint_interval_ms; /* 0 turns off the timer */
    const ps_vfs *vfs; /* NULL selects POSIX */
    ps_logger logger; /* zeroed is off */
} mq_config;

/* Defaults to a cap of one million messages and 1 MiB batches. */
void mq_config_init(mq_config *cfg);

/*
 * Opens the queue in dir and replays its WAL. PS_CREATE creates the store
 * and queue if needed. Errors come from ps_open and ps_wal_open, plus
 * EINVAL for a bad config.
 */
int mq_open(const char *dir, const mq_config *cfg, u32 flags, mq **out);

/* Close every reader first. Call mq_checkpoint first to skip replay at the next open. */
void mq_close(mq *q);

/* Writes committed pages to the store, so the next open has less to replay. */
int mq_checkpoint(mq *q);

/* For inspection with the msglog.h read functions and a snapshot. */
ps_store *mq_store(mq *q);
const ml_log *mq_log(mq *q);

typedef struct mq_msg {
    const void *data;
    u32 len;
} mq_msg;

/*
 * Appends a message and returns its sequence number once it is durable.
 * EMSGSIZE if it is longer than max_batch.
 */
int mq_push(mq *q, const void *msg, u32 len, u64 *seq);

/*
 * Appends n messages in one operation, numbered from *first. EINVAL for
 * no messages or more than MQ_BATCH_MAX. EMSGSIZE if their payload is
 * longer than max_batch. ENOBUFS if the batch needs more pages than one
 * operation may hold in the store's cache.
 */
int mq_push_batch(mq *q, const mq_msg *msgs, u32 n, u64 *first);

/*
 * Adds a reader whose first message is pos, clamped to the kept messages.
 * MQ_OLDEST starts at the oldest kept message, MQ_LATEST at the next new
 * one. EINVAL for a bad name, EEXIST if it is taken, ENOSPC when the
 * reader table is full.
 */
int mq_subscribe(mq *q, const char *name, u64 pos);

/* ENOENT if the reader is missing, EBUSY while a handle has it open. */
int mq_unsubscribe(mq *q, const char *name);

/* Caller-owned. The fields are private to mq.c. */
typedef struct mq_reader {
    mq *q;
    struct mq_reader *next_open; /* the queue's list of open handles */
    char name[MQ_NAME_MAX + 1];
    u64 next; /* the next message to deliver */
    u64 acked; /* durable position */
    u64 missed; /* trimmed messages not yet reported */
    u64 limit; /* snap holds the messages below this */
    ps_snapshot *snap;
} mq_reader;

/* A message delivered by mq_read. */
typedef struct mq_item {
    u64 seq;
    u32 len;
    u64 missed; /* messages trimmed away just before this one */
} mq_item;

/*
 * Opens a handle that delivers from the reader's acknowledged position.
 * ENOENT if the reader is missing, EBUSY if another handle has it open.
 * Use a handle from one thread at a time.
 */
int mq_reader_open(mq *q, const char *name, mq_reader *r);
void mq_reader_close(mq_reader *r);

/*
 * Delivers the next durable message. It waits up to timeout_ms for one, or
 * forever if timeout_ms is negative, then returns ETIMEDOUT. EMSGSIZE
 * means buf is shorter than the message: out->len holds its length, and
 * the next call delivers it again.
 */
int mq_read(mq_reader *r, void *buf, u32 cap, i64 timeout_ms, mq_item *out);

/*
 * Acknowledges every message up to and including seq. Durable on return.
 * Acknowledging beyond the delivered messages skips the ones in between.
 * EINVAL if seq isn't durable yet.
 */
int mq_ack(mq_reader *r, u64 seq);

PS_EXTERN_C_END
