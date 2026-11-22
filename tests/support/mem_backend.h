#pragma once

/*
 * In-memory ps_backend for unit tests. Its durable state (the header info
 * and the page images) lives in the mem_backend object and survives
 * ps_close, so a test can close and reopen a store on it.
 *
 * It checks the store: every ckpt_write page must carry a valid checksum
 * and its own page_id, ckpt_commit must follow exactly count writes, and
 * the ckpt_* calls must come in order (aborts the test otherwise).
 * All functions are thread-safe; mb_page's pointer is stable until
 * mb_destroy but its contents change at each committed checkpoint.
 */

#include "pagestore/backend.h"
#include "pagestore/pagestore.h"
#include "pagestore/types.h"

typedef struct mem_backend mem_backend;

/* Injection points for mb_fail. */
typedef enum mb_point {
    MB_READ, /* read_page */
    MB_WRITE, /* ckpt_write */
    MB_COMMIT, /* ckpt_commit, non-fatal */
    MB_COMMIT_FATAL, /* ckpt_commit, fatal */
    MB_POINTS
} mb_point;

/* The backend starts unformatted. Opening it requires PS_CREATE. */
mem_backend *mb_create(void);
void mb_destroy(mem_backend *mb);

/* Use with ps_open_backend when a test needs a custom config. */
void mb_backend(mem_backend *mb, ps_backend *out);

/* Uses 4096-byte pages and manual checkpoints. */
ps_config mb_config(u32 frames);

/* ps_open_backend on mb with mb_config(frames). */
int mb_open(mem_backend *mb, u32 frames, u32 flags, ps_store **out);

/*
 * Returns NULL for an unwritten page. The pointer lives until mb_destroy,
 * but checkpoint commits can replace its bytes.
 */
const u8 *mb_page(mem_backend *mb, u32 id);
/* Copies a full page using the backend header's page size. */
void mb_put(mem_backend *mb, u32 id, const void *page);

/* Also marks the backend formatted. */
void mb_set_info(mem_backend *mb, ps_backend_info info);
ps_backend_info mb_info(mem_backend *mb);

u64 mb_ckpt_count(mem_backend *mb);
bool mb_written_last(mem_backend *mb, u32 id);

/* Fail the next count calls at p. Use -1 to fail until mb_fail(mb, p, 0, 0). */
void mb_fail(mem_backend *mb, mb_point p, int err, i32 count);
/* While on, ckpt_write blocks. */
void mb_block_writes(mem_backend *mb, bool on);
/* ckpt_commit sleeps this long before committing. */
void mb_set_commit_delay_ms(mem_backend *mb, u32 ms);

/* Seal a page before mb_put when the store should accept it on page-in. */
void mb_seal_page(void *page, u32 page_size);
