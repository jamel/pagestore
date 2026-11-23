#pragma once

/*
 * A message log keeps byte-string messages under consecutive sequence
 * numbers in PageStore pages. It holds at most cap of the newest messages
 * and remembers a position for each named reader.
 *
 * Payloads are packed end to end in one logical byte space made of data
 * pages, so a message can span pages. Two radix arrays (radix.h) find
 * them: the index maps a sequence number to the message's byte offset, and
 * the page map maps a logical page number to its page ID.
 *
 *     index      seq:     5      6      7
 *                         |      |      |
 *     bytes          [ .. msg 5 .. | msg 6 | .. msg 7 ........ ]
 *     pages          |  logical 2  |  logical 3  |  logical 4  |
 *     page map           -> 18         -> 9          -> 31
 *
 * A push beyond the cap drops the oldest message and frees the pages only
 * it used, whatever the readers' positions. A reader's position is the
 * next message it needs and only moves forward. If the oldest kept message
 * is above it, the reader has missed the ones in between.
 *
 * Changes run inside the caller's store operation, so the caller picks the
 * LSN, logs the change and commits or aborts. Abort after any failed
 * change, because it can leave the log half changed. One thread changes
 * the log at a time. Readers on other threads can share the ml_log, each
 * with its own snapshot.
 *
 * Example: read every kept message through a snapshot.
 *
 *     ml_state st;
 *     int rv = ml_state_get(&l, snap, &st);
 *     if (rv != 0) {
 *         return rv;
 *     }
 *     for (u64 seq = st.first_seq; seq < st.next_seq; seq++) {
 *         u32 len;
 *         rv = ml_read(&l, snap, seq, buf, sizeof(buf), &len);
 *         if (rv != 0) {
 *             break;
 *         }
 *     }
 */

#include "pagestore/pagestore.h"
#include "radix.h"

PS_EXTERN_C_BEGIN

/* Reader names are 1 to ML_NAME_MAX bytes and contain no NUL. */
#define ML_NAME_MAX 23u

/* Set up by ml_open. Callers may read the fields but not change them. */
typedef struct ml_log {
    ps_store *store;
    ps_page_id meta;
    u32 page_size;
    u32 data_size; /* payload bytes per data page */
    u32 max_readers;
    rx_array index; /* geometry only, the root lives in the meta page */
    rx_array pages;
} ml_log;

/* The log's counters as of one snapshot. */
typedef struct ml_state {
    u64 cap;
    u64 first_seq; /* oldest message kept, equal to next_seq when empty */
    u64 next_seq; /* the number the next push gets */
    u64 bytes; /* payload bytes of the kept messages */
    u32 readers;
} ml_state;

/* Filled in by ml_check. */
typedef struct ml_stats {
    u64 messages;
    u64 bytes;
    u32 readers;
    u64 data_pages;
    u64 index_pages;
    u64 map_pages;
} ml_stats;

/*
 * Allocates an empty log in the current operation and returns its meta
 * page ID, which later opens need. cap is the number of messages kept, at
 * least 1.
 */
int ml_create(ps_store *s, u64 cap, ps_page_id *meta);

/*
 * Sets up a handle for the log whose meta page is meta. It reads no pages,
 * so it works before a replayed operation creates the log.
 */
void ml_open(ps_store *s, ps_page_id meta, ml_log *l);

/*
 * Appends a message in the current operation and returns its sequence
 * number. If that leaves more than cap messages, the oldest is dropped.
 */
int ml_push(ml_log *l, const void *msg, u32 len, u64 *seq);

int ml_state_get(const ml_log *l, ps_snapshot *snap, ml_state *out);

/*
 * Copies up to cap bytes of message seq into buf and sets *len to its full
 * length. ENOENT if seq isn't kept.
 */
int ml_read(const ml_log *l, ps_snapshot *snap, u64 seq, void *buf, u32 cap, u32 *len);

/*
 * Adds a reader in the current operation. pos is clamped to the kept
 * range, so 0 starts at the oldest message and UINT64_MAX at the next new
 * one. EINVAL for a bad name, EEXIST if the name is taken, ENOSPC if the
 * reader table is full.
 */
int ml_reader_add(ml_log *l, const char *name, u64 pos);

/* Removes a reader in the current operation. ENOENT if it is missing. */
int ml_reader_remove(ml_log *l, const char *name);

/*
 * Moves a reader's position forward to pos in the current operation. A
 * position at or below the current one changes nothing. EINVAL if pos is
 * above next_seq, ENOENT if the reader is missing.
 */
int ml_reader_ack(ml_log *l, const char *name, u64 pos);

/* ENOENT if the reader is missing. */
int ml_reader_get(const ml_log *l, ps_snapshot *snap, const char *name, u64 *pos);

/*
 * Lists readers. Start with *slot at 0. Each call returns the next reader
 * at or after *slot and sets *slot past it. ENOENT after the last one.
 */
int ml_reader_next(
    const ml_log *l,
    ps_snapshot *snap,
    u32 *slot,
    char name[ML_NAME_MAX + 1],
    u64 *pos);

/*
 * Checks the counters, both radix arrays, every message offset, every
 * data page and the reader table, and fills in *out. A broken log returns
 * EBADMSG and describes the first problem in msg. Reads every index
 * entry, so it takes time in proportion to the kept messages.
 */
int ml_check(const ml_log *l, ps_snapshot *snap, ml_stats *out, char *msg, usize msg_cap);

PS_EXTERN_C_END
