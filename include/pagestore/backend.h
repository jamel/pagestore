#pragma once

/*
 * A backend stores the page images and the header for the last durable
 * checkpoint. A checkpoint calls begin, writes the promised number of
 * pages, then commits. A crash or fatal error during commit can leave a
 * partly updated disk image, so the next open must recover a consistent
 * image.
 *
 * Integer callbacks return 0 or a positive errno. All receive the
 * backend's ctx. Reads can overlap a checkpoint. Checkpoints are
 * serialized, and open and close run alone.
 */

#include "pagestore/compiler.h"
#include "pagestore/types.h"

PS_EXTERN_C_BEGIN

/* The durable header returned by open. */
typedef struct ps_backend_info {
    u32 page_size;
    u32 num_groups;
    u64 seq; /* zero before the first checkpoint */
    u64 durable_lsn;
} ps_backend_info;

/* Defined by the backend. */
typedef struct ps_backend_ctx ps_backend_ctx;

typedef struct ps_backend {
    ps_backend_ctx *ctx;
    /*
     * Called at most once per context. With create, format a missing store with
     * seq = lsn = 0 and one group. page_size is used only for formatting,
     * where 0 means 4096 and an invalid size is EINVAL. Return an existing
     * header even if seq is 0. ENOENT for a missing store without create,
     * EBUSY when already open, or EBADMSG for an unreadable format.
     */
    int (*open)(ps_backend_ctx *ctx, bool create, u32 page_size, ps_backend_info *out);
    /* Fill one page-sized buffer. Zero-fill any part past EOF. */
    int (*read_page)(ps_backend_ctx *ctx, u32 id, void *buf);
    /* The caller must write exactly count pages before committing. Zero is valid. */
    int (*ckpt_begin)(ps_backend_ctx *ctx, u64 lsn, u32 num_groups, u32 count);
    /* The caller sets the page checksum and can reuse the buffer after this call. */
    int (*ckpt_write)(ps_backend_ctx *ctx, u32 id, const void *page);
    /*
     * A successful return means the checkpoint is durable. On error, a
     * true *fatal means reopen to recover. Otherwise abort before retrying.
     */
    int (*ckpt_commit)(ps_backend_ctx *ctx, bool *fatal);
    /* Call after a successful begin if a write or commit fails nonfatally. */
    void (*ckpt_abort)(ps_backend_ctx *ctx);
    /* Release backend resources. May be called before open or after it fails. */
    void (*close)(ps_backend_ctx *ctx);
} ps_backend;

PS_EXTERN_C_END
