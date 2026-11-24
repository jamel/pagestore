#pragma once

/*
 * Disk layouts and codecs for the data file, checkpoint journal and WAL.
 * Integers are little-endian. Supply a full encoded object to fixed-size
 * decoders. Only the WAL record decoder takes a byte count. Decoders
 * return 0 on success or EBADMSG for malformed input.
 */

#include "pagestore/types.h"

#define PS_PAGE_SIZE_MIN 4096u
#define PS_PAGE_SIZE_MAX 65536u

/* A power of two in [PS_PAGE_SIZE_MIN, PS_PAGE_SIZE_MAX]. */
bool ps_page_size_valid(u32 page_size);

/*
 * Data file header at byte 0 (44 bytes).
 *
 * Offset   +0..3                  +4..7
 *       +---------------------------------------------+
 *  0x00 | magic "PSDATA01"                            |
 *       +----------------------+----------------------+
 *  0x08 | version = 1          | page_size            |
 *       +----------------------+----------------------+
 *  0x10 | num_groups           | reserved = 0         |
 *       +----------------------+----------------------+
 *  0x18 | seq                                         |
 *       +---------------------------------------------+
 *  0x20 | durable_lsn                                 |
 *       +----------------------+----------------------+
 *  0x28 | CRC32C               |
 *       +----------------------+
 *
 * CRC32C covers bytes 0x00..0x27.
 */
typedef struct ps_data_header {
    u32 page_size;
    u32 num_groups;
    u64 seq;
    ps_lsn durable_lsn;
} ps_data_header;

#define PS_DATA_HEADER_SIZE 44

void ps_data_header_encode(const ps_data_header *h, u8 out[PS_DATA_HEADER_SIZE]);

/* EBADMSG for an invalid header, including a zero group count. */
int ps_data_header_decode(const u8 *buf, ps_data_header *h);

/*
 * Journal header at byte 0 (48 bytes).
 *
 * Offset   +0..3                  +4..7
 *       +---------------------------------------------+
 *  0x00 | magic "PSJRNL01"                            |
 *       +----------------------+----------------------+
 *  0x08 | version = 1          | page_size            |
 *       +----------------------+----------------------+
 *  0x10 | seq                                         |
 *       +---------------------------------------------+
 *  0x18 | lsn                                         |
 *       +----------------------+----------------------+
 *  0x20 | num_groups           | count                |
 *       +----------------------+----------------------+
 *  0x28 | reserved = 0                                |
 *       +---------------------------------------------+
 *
 * The header is followed by count entries and the trailer.
 */
typedef struct ps_journal_header {
    u32 page_size;
    u32 num_groups;
    u32 count;
    u64 seq;
    ps_lsn lsn;
} ps_journal_header;

#define PS_JOURNAL_HEADER_SIZE 48
#define PS_JOURNAL_ENTRY_HEADER_SIZE 8
#define PS_JOURNAL_TRAILER_SIZE 8

void ps_journal_header_encode(const ps_journal_header *h, u8 out[PS_JOURNAL_HEADER_SIZE]);

/* A checkpoint may have zero entries. */
int ps_journal_header_decode(const u8 *buf, ps_journal_header *h);

/* Pass count to get the trailer offset. */
u64 ps_journal_entry_off(const ps_journal_header *h, u32 i);

/* Total file size including the header and trailer. */
u64 ps_journal_size(const ps_journal_header *h);

/*
 * Journal entry at ps_journal_entry_off(h, i).
 *
 * Offset   +0..3                  +4..7
 *       +----------------------+----------------------+
 *  0x00 | page_id              | reserved = 0         |
 *       +----------------------+----------------------+
 *
 * The 8-byte entry header is followed by page_size bytes of page data.
 */
void ps_journal_entry_header_encode(ps_page_id page_id, u8 out[PS_JOURNAL_ENTRY_HEADER_SIZE]);

/* EBADMSG if the reserved word is nonzero. */
int ps_journal_entry_header_decode(const u8 *buf, ps_page_id *page_id);

/*
 * Journal trailer at ps_journal_entry_off(h, h->count).
 *
 * Offset   +0..3                  +4..7
 *       +----------------------+----------------------+
 *  0x00 | CRC32C               | magic "PSJT"         |
 *       +----------------------+----------------------+
 *
 * CRC32C covers all bytes before the trailer.
 */
void ps_journal_trailer_encode(u32 crc, u8 out[PS_JOURNAL_TRAILER_SIZE]);

/* Reads the encoded CRC. The caller verifies it against the journal bytes. */
int ps_journal_trailer_decode(const u8 *buf, u32 *crc);

/*
 * WAL segment header at byte 0 (40 bytes).
 *
 * Offset   +0..3                  +4..7
 *       +---------------------------------------------+
 *  0x00 | magic "PSWALS01"                            |
 *       +----------------------+----------------------+
 *  0x08 | version = 1          | reserved = 0         |
 *       +----------------------+----------------------+
 *  0x10 | seq                                         |
 *       +---------------------------------------------+
 *  0x18 | prev_lsn                                    |
 *       +----------------------+----------------------+
 *  0x20 | CRC32C               | reserved = 0         |
 *       +----------------------+----------------------+
 *
 * CRC32C covers bytes 0x00..0x1F.
 */
typedef struct ps_wal_seg_header {
    u64 seq;
    ps_lsn prev_lsn;
} ps_wal_seg_header;

#define PS_WAL_SEG_HEADER_SIZE 40

void ps_wal_seg_header_encode(const ps_wal_seg_header *h, u8 out[PS_WAL_SEG_HEADER_SIZE]);

/* EBADMSG for a bad header. The reserved word at 0x24 is not checked. */
int ps_wal_seg_header_decode(const u8 *buf, ps_wal_seg_header *h);

/*
 * WAL record at an 8-byte-aligned file offset.
 *
 * Offset   +0..3                  +4..7
 *       +----------------------+----------------------+
 *  0x00 | len                  | CRC32C               |
 *       +----------------------+----------------------+
 *  0x08 | lsn                                         |
 *       +---------------------------------------------+
 *
 * 0x10         payload (len bytes)
 * 0x10 + len   0..7 zero bytes to reach 8-byte alignment
 *
 * CRC32C covers the encoded lsn followed by the payload.
 */
#define PS_WAL_REC_HEADER_SIZE 16

/* Record header plus payload rounded up to an 8-byte boundary. */
usize ps_wal_rec_size(u32 len);

/* Supply ps_wal_rec_size(len) bytes of output. A zero-length payload may be NULL. */
void ps_wal_rec_encode(u8 *out, ps_lsn lsn, const void *payload, u32 len);

/*
 * *payload points into buf. *size includes the padding so the caller can
 * step to the next record. EBADMSG leaves the outputs untouched.
 */
int ps_wal_rec_decode(
    const u8 *buf,
    usize avail,
    ps_lsn after_lsn,
    ps_lsn *lsn,
    const u8 **payload,
    u32 *len,
    usize *size);
