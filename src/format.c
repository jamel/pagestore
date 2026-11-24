/*
 * Codecs use fixed byte offsets, never struct layout. Decoders write their
 * outputs only after every check passes.
 */

#include "crc32c.h"
#include "format.h"
#include "util.h"

#include "pagestore/bytes.h"

#include <errno.h>
#include <string.h>

#define PS_FORMAT_VERSION 1u

static const char ps_data_magic[8] = {'P', 'S', 'D', 'A', 'T', 'A', '0', '1'};
static const char ps_journal_magic[8] = {'P', 'S', 'J', 'R', 'N', 'L', '0', '1'};
static const char ps_journal_trailer_magic[4] = {'P', 'S', 'J', 'T'};
static const char ps_wal_seg_magic[8] = {'P', 'S', 'W', 'A', 'L', 'S', '0', '1'};

bool ps_page_size_valid(u32 page_size)
{
    return ps_is_pow2(page_size) && page_size >= PS_PAGE_SIZE_MIN && page_size <= PS_PAGE_SIZE_MAX;
}

/* ---- data header ---- */

void ps_data_header_encode(const ps_data_header *h, u8 out[PS_DATA_HEADER_SIZE])
{
    memcpy(out, ps_data_magic, 8);
    ps_put_le32(out + 8, PS_FORMAT_VERSION);
    ps_put_le32(out + 12, h->page_size);
    ps_put_le32(out + 16, h->num_groups);
    ps_put_le32(out + 20, 0);
    ps_put_le64(out + 24, h->seq);
    ps_put_le64(out + 32, h->durable_lsn.raw);
    ps_put_le32(out + 40, ps_crc32c(0, out, 40));
}

int ps_data_header_decode(const u8 *buf, ps_data_header *h)
{
    if (memcmp(buf, ps_data_magic, 8) != 0) {
        return EBADMSG;
    }
    if (ps_get_le32(buf + 40) != ps_crc32c(0, buf, 40)) {
        return EBADMSG;
    }
    if (ps_get_le32(buf + 8) != PS_FORMAT_VERSION || ps_get_le32(buf + 20) != 0) {
        return EBADMSG;
    }
    const u32 page_size = ps_get_le32(buf + 12);
    const u32 num_groups = ps_get_le32(buf + 16);
    if (!ps_page_size_valid(page_size) || num_groups == 0) {
        return EBADMSG;
    }
    h->page_size = page_size;
    h->num_groups = num_groups;
    h->seq = ps_get_le64(buf + 24);
    h->durable_lsn = ps_lsn_from(ps_get_le64(buf + 32));
    return 0;
}

/* ---- journal ---- */

void ps_journal_header_encode(const ps_journal_header *h, u8 out[PS_JOURNAL_HEADER_SIZE])
{
    memcpy(out, ps_journal_magic, 8);
    ps_put_le32(out + 8, PS_FORMAT_VERSION);
    ps_put_le32(out + 12, h->page_size);
    ps_put_le64(out + 16, h->seq);
    ps_put_le64(out + 24, h->lsn.raw);
    ps_put_le32(out + 32, h->num_groups);
    ps_put_le32(out + 36, h->count);
    ps_put_le64(out + 40, 0);
}

int ps_journal_header_decode(const u8 *buf, ps_journal_header *h)
{
    if (memcmp(buf, ps_journal_magic, 8) != 0) {
        return EBADMSG;
    }
    if (ps_get_le32(buf + 8) != PS_FORMAT_VERSION || ps_get_le64(buf + 40) != 0) {
        return EBADMSG;
    }
    const u32 page_size = ps_get_le32(buf + 12);
    const u32 num_groups = ps_get_le32(buf + 32);
    if (!ps_page_size_valid(page_size) || num_groups == 0) {
        return EBADMSG;
    }
    h->page_size = page_size;
    h->num_groups = num_groups;
    h->count = ps_get_le32(buf + 36);
    h->seq = ps_get_le64(buf + 16);
    h->lsn = ps_lsn_from(ps_get_le64(buf + 24));
    return 0;
}

u64 ps_journal_entry_off(const ps_journal_header *h, u32 i)
{
    const u64 stride = (u64) PS_JOURNAL_ENTRY_HEADER_SIZE + h->page_size;
    return (u64) PS_JOURNAL_HEADER_SIZE + (u64) i * stride;
}

u64 ps_journal_size(const ps_journal_header *h)
{
    return ps_journal_entry_off(h, h->count) + PS_JOURNAL_TRAILER_SIZE;
}

void ps_journal_entry_header_encode(ps_page_id page_id, u8 out[PS_JOURNAL_ENTRY_HEADER_SIZE])
{
    ps_put_le32(out, page_id.raw);
    ps_put_le32(out + 4, 0);
}

int ps_journal_entry_header_decode(const u8 *buf, ps_page_id *page_id)
{
    if (ps_get_le32(buf + 4) != 0) {
        return EBADMSG;
    }
    *page_id = ps_page_id_from(ps_get_le32(buf));
    return 0;
}

void ps_journal_trailer_encode(u32 crc, u8 out[PS_JOURNAL_TRAILER_SIZE])
{
    ps_put_le32(out, crc);
    memcpy(out + 4, ps_journal_trailer_magic, 4);
}

int ps_journal_trailer_decode(const u8 *buf, u32 *crc)
{
    if (memcmp(buf + 4, ps_journal_trailer_magic, 4) != 0) {
        return EBADMSG;
    }
    *crc = ps_get_le32(buf);
    return 0;
}

/* ---- WAL segment header ---- */

void ps_wal_seg_header_encode(const ps_wal_seg_header *h, u8 out[PS_WAL_SEG_HEADER_SIZE])
{
    memcpy(out, ps_wal_seg_magic, 8);
    ps_put_le32(out + 8, PS_FORMAT_VERSION);
    ps_put_le32(out + 12, 0);
    ps_put_le64(out + 16, h->seq);
    ps_put_le64(out + 24, h->prev_lsn.raw);
    ps_put_le32(out + 32, ps_crc32c(0, out, 32));
    ps_put_le32(out + 36, 0);
}

int ps_wal_seg_header_decode(const u8 *buf, ps_wal_seg_header *h)
{
    if (memcmp(buf, ps_wal_seg_magic, 8) != 0) {
        return EBADMSG;
    }
    if (ps_get_le32(buf + 32) != ps_crc32c(0, buf, 32)) {
        return EBADMSG;
    }
    if (ps_get_le32(buf + 8) != PS_FORMAT_VERSION || ps_get_le32(buf + 12) != 0) {
        return EBADMSG;
    }
    h->seq = ps_get_le64(buf + 16);
    h->prev_lsn = ps_lsn_from(ps_get_le64(buf + 24));
    return 0;
}

/* ---- WAL record ---- */

usize ps_wal_rec_size(u32 len)
{
    return (usize) PS_WAL_REC_HEADER_SIZE + (((usize) len + 7) & ~(usize) 7);
}

static u32 ps_wal_rec_crc(ps_lsn lsn, const void *payload, u32 len)
{
    u8 lsn_le[8];
    ps_put_le64(lsn_le, lsn.raw);
    return ps_crc32c(ps_crc32c(0, lsn_le, sizeof(lsn_le)), payload, len);
}

void ps_wal_rec_encode(u8 *out, ps_lsn lsn, const void *payload, u32 len)
{
    const usize size = ps_wal_rec_size(len);
    ps_put_le32(out, len);
    ps_put_le32(out + 4, ps_wal_rec_crc(lsn, payload, len));
    ps_put_le64(out + 8, lsn.raw);
    if (len != 0) {
        memcpy(out + PS_WAL_REC_HEADER_SIZE, payload, len);
    }
    memset(out + PS_WAL_REC_HEADER_SIZE + len, 0, size - PS_WAL_REC_HEADER_SIZE - len);
}

int ps_wal_rec_decode(
    const u8 *buf,
    usize avail,
    ps_lsn after_lsn,
    ps_lsn *lsn,
    const u8 **payload,
    u32 *len,
    usize *size)
{
    if (avail < PS_WAL_REC_HEADER_SIZE) {
        return EBADMSG;
    }
    const u32 n = ps_get_le32(buf);
    /* Check the unpadded length before computing the size in usize. */
    if ((u64) PS_WAL_REC_HEADER_SIZE + n > (u64) avail) {
        return EBADMSG;
    }
    const usize sz = ps_wal_rec_size(n);
    if (sz > avail) {
        return EBADMSG;
    }
    const ps_lsn l = ps_lsn_from(ps_get_le64(buf + 8));
    if (ps_lsn_le(l, after_lsn)) {
        return EBADMSG;
    }
    const u8 *p = buf + PS_WAL_REC_HEADER_SIZE;
    if (ps_get_le32(buf + 4) != ps_wal_rec_crc(l, p, n)) {
        return EBADMSG;
    }
    for (usize i = PS_WAL_REC_HEADER_SIZE + n; i < sz; i++) {
        if (buf[i] != 0) {
            return EBADMSG;
        }
    }
    *lsn = l;
    *payload = p;
    *len = n;
    *size = sz;
    return 0;
}
