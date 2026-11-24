#include "crc32c.h"
#include "format.h"
#include "pagestore/bytes.h"
#include "test.h"

#include <errno.h>
#include <string.h>

static void test_data_header_layout(void)
{
    const ps_data_header h = {
        .page_size = 8192,
        .num_groups = 3,
        .seq = 0x0102030405060708ull,
        .durable_lsn = 0x1112131415161718ull,
    };
    u8 b[PS_DATA_HEADER_SIZE];

    PS_STATIC_ASSERT(PS_DATA_HEADER_SIZE == 44, "data header is 44 bytes");
    ps_data_header_encode(&h, b);
    T_MEMEQ(b, "PSDATA01", 8);
    T_EQ(ps_get_le32(b + 8), 1);
    T_EQ(ps_get_le32(b + 12), 8192);
    T_EQ(ps_get_le32(b + 16), 3);
    T_EQ(ps_get_le32(b + 20), 0);
    T_EQ(ps_get_le64(b + 24), 0x0102030405060708ull);
    T_EQ(ps_get_le64(b + 32), 0x1112131415161718ull);
    T_EQ(ps_get_le32(b + 40), ps_crc32c(0, b, 40));
}

static void test_data_header_roundtrip_and_corruption(void)
{
    ps_data_header h = {.page_size = 4096, .num_groups = 3, .seq = 7, .durable_lsn = 1234};
    ps_data_header g;
    u8 b[PS_DATA_HEADER_SIZE];

    ps_data_header_encode(&h, b);
    T_OK(ps_data_header_decode(b, &g));
    T_MEMEQ(&h, &g, sizeof(h));

    for (int i = 0; i < PS_DATA_HEADER_SIZE; i++) {
        b[i] ^= 1;
        T_ERR(ps_data_header_decode(b, &g), EBADMSG);
        b[i] ^= 1;
    }
    T_OK(ps_data_header_decode(b, &g));

    for (u32 ps = 4096; ps <= 65536; ps *= 2) {
        h.page_size = ps;
        ps_data_header_encode(&h, b);
        T_OK(ps_data_header_decode(b, &g));
        T_EQ(g.page_size, ps);
    }

    /* Refresh the CRC to isolate validation of each field. */
    h.page_size = 2048;
    ps_data_header_encode(&h, b);
    T_ERR(ps_data_header_decode(b, &g), EBADMSG);
    h.page_size = 6000;
    ps_data_header_encode(&h, b);
    T_ERR(ps_data_header_decode(b, &g), EBADMSG);
    h.page_size = 131072;
    ps_data_header_encode(&h, b);
    T_ERR(ps_data_header_decode(b, &g), EBADMSG);
    h.page_size = 4096;
    h.num_groups = 0;
    ps_data_header_encode(&h, b);
    T_ERR(ps_data_header_decode(b, &g), EBADMSG);

    h.num_groups = 1;
    ps_data_header_encode(&h, b);
    ps_put_le32(b + 8, 2);
    ps_put_le32(b + 40, ps_crc32c(0, b, 40));
    T_ERR(ps_data_header_decode(b, &g), EBADMSG);
    ps_data_header_encode(&h, b);
    ps_put_le32(b + 20, 1);
    ps_put_le32(b + 40, ps_crc32c(0, b, 40));
    T_ERR(ps_data_header_decode(b, &g), EBADMSG);

    memset(b, 0, sizeof(b));
    T_ERR(ps_data_header_decode(b, &g), EBADMSG);
}

static void test_journal_layout(void)
{
    ps_journal_header h = {.page_size = 4096, .num_groups = 2, .count = 3, .seq = 9, .lsn = 77};
    ps_journal_header g;
    u8 b[PS_JOURNAL_HEADER_SIZE];
    u8 t[PS_JOURNAL_TRAILER_SIZE];
    u8 e[PS_JOURNAL_ENTRY_HEADER_SIZE];
    u32 crc = 0;
    ps_page_id id = PS_PAGE_ID_NONE;

    PS_STATIC_ASSERT(PS_JOURNAL_HEADER_SIZE == 48, "journal header is 48 bytes");
    PS_STATIC_ASSERT(PS_JOURNAL_TRAILER_SIZE == 8, "journal trailer is 8 bytes");
    T_EQ(ps_journal_entry_off(&h, 0), 48);
    T_EQ(ps_journal_entry_off(&h, 1), 4152);
    T_EQ(ps_journal_entry_off(&h, 3), 48 + 3 * 4104);
    T_EQ(ps_journal_size(&h), 48 + 3 * 4104 + 8);

    h.page_size = 65536;
    h.count = 0xFFFFFFFFu;
    T_EQ(ps_journal_entry_off(&h, 0xFFFFFFFEu), 48 + 0xFFFFFFFEull * (8 + 65536));
    T_EQ(ps_journal_size(&h), 48 + 0xFFFFFFFFull * (8 + 65536) + 8);
    h.page_size = 4096;
    h.count = 3;

    ps_journal_header_encode(&h, b);
    T_MEMEQ(b, "PSJRNL01", 8);
    T_EQ(ps_get_le32(b + 8), 1);
    T_EQ(ps_get_le32(b + 12), 4096);
    T_EQ(ps_get_le64(b + 16), 9);
    T_EQ(ps_get_le64(b + 24), 77);
    T_EQ(ps_get_le32(b + 32), 2);
    T_EQ(ps_get_le32(b + 36), 3);
    T_EQ(ps_get_le64(b + 40), 0);
    T_OK(ps_journal_header_decode(b, &g));
    T_EQ(g.page_size, 4096);
    T_EQ(g.num_groups, 2);
    T_EQ(g.count, 3);
    T_EQ(g.seq, 9);
    T_EQ(g.lsn, 77);

    h.count = 0;
    ps_journal_header_encode(&h, b);
    T_OK(ps_journal_header_decode(b, &g));
    T_EQ(g.count, 0);
    T_EQ(ps_journal_size(&h), 56);
    h.count = 3;

    ps_journal_header_encode(&h, b);
    b[0] ^= 1;
    T_ERR(ps_journal_header_decode(b, &g), EBADMSG);
    ps_journal_header_encode(&h, b);
    b[7] ^= 1;
    T_ERR(ps_journal_header_decode(b, &g), EBADMSG);
    ps_journal_header_encode(&h, b);
    ps_put_le32(b + 8, 2);
    T_ERR(ps_journal_header_decode(b, &g), EBADMSG);
    ps_journal_header_encode(&h, b);
    ps_put_le32(b + 12, 6000);
    T_ERR(ps_journal_header_decode(b, &g), EBADMSG);
    ps_journal_header_encode(&h, b);
    ps_put_le32(b + 12, 2048);
    T_ERR(ps_journal_header_decode(b, &g), EBADMSG);
    ps_journal_header_encode(&h, b);
    ps_put_le32(b + 32, 0);
    T_ERR(ps_journal_header_decode(b, &g), EBADMSG);
    ps_journal_header_encode(&h, b);
    b[44] = 1;
    T_ERR(ps_journal_header_decode(b, &g), EBADMSG);
    memset(b, 0, sizeof(b));
    T_ERR(ps_journal_header_decode(b, &g), EBADMSG);

    PS_STATIC_ASSERT(PS_JOURNAL_ENTRY_HEADER_SIZE == 8, "journal entry header is 8 bytes");
    ps_journal_entry_header_encode(ps_page_id_from(0xFFFFFFFEu), e);
    T_EQ(ps_get_le32(e), 0xFFFFFFFEu);
    T_EQ(ps_get_le32(e + 4), 0);
    T_OK(ps_journal_entry_header_decode(e, &id));
    T_EQ(id.raw, 0xFFFFFFFEu);
    e[5] = 1;
    T_ERR(ps_journal_entry_header_decode(e, &id), EBADMSG);

    ps_journal_trailer_encode(0xDEADBEEFu, t);
    T_EQ(ps_get_le32(t), 0xDEADBEEFu);
    T_MEMEQ(t + 4, "PSJT", 4);
    T_OK(ps_journal_trailer_decode(t, &crc));
    T_EQ(crc, 0xDEADBEEFu);
    for (int i = 4; i < PS_JOURNAL_TRAILER_SIZE; i++) {
        t[i] ^= 1;
        T_ERR(ps_journal_trailer_decode(t, &crc), EBADMSG);
        t[i] ^= 1;
    }
    memset(t, 0, sizeof(t));
    T_ERR(ps_journal_trailer_decode(t, &crc), EBADMSG);
}

static void test_wal_header_roundtrip(void)
{
    const ps_wal_seg_header h = {.seq = 0x0102030405060708ull, .prev_lsn = 0xA1A2A3A4A5A6A7A8ull};
    ps_wal_seg_header g;
    u8 b[PS_WAL_SEG_HEADER_SIZE];

    PS_STATIC_ASSERT(PS_WAL_SEG_HEADER_SIZE == 40, "WAL segment header is 40 bytes");
    ps_wal_seg_header_encode(&h, b);
    T_MEMEQ(b, "PSWALS01", 8);
    T_EQ(ps_get_le32(b + 8), 1);
    T_EQ(ps_get_le32(b + 12), 0);
    T_EQ(ps_get_le64(b + 16), h.seq);
    T_EQ(ps_get_le64(b + 24), h.prev_lsn);
    T_EQ(ps_get_le32(b + 32), ps_crc32c(0, b, 32));
    T_EQ(ps_get_le32(b + 36), 0);

    T_OK(ps_wal_seg_header_decode(b, &g));
    T_MEMEQ(&h, &g, sizeof(h));

    for (int i = 0; i < 36; i++) {
        b[i] ^= 1;
        T_ERR(ps_wal_seg_header_decode(b, &g), EBADMSG);
        b[i] ^= 1;
    }
    T_OK(ps_wal_seg_header_decode(b, &g));

    /* Keep the checksum valid while changing the version and reserved field. */
    ps_put_le32(b + 8, 2);
    ps_put_le32(b + 32, ps_crc32c(0, b, 32));
    T_ERR(ps_wal_seg_header_decode(b, &g), EBADMSG);
    ps_wal_seg_header_encode(&h, b);
    ps_put_le32(b + 12, 1);
    ps_put_le32(b + 32, ps_crc32c(0, b, 32));
    T_ERR(ps_wal_seg_header_decode(b, &g), EBADMSG);

    memset(b, 0, sizeof(b));
    T_ERR(ps_wal_seg_header_decode(b, &g), EBADMSG);
}

static void test_wal_rec_size(void)
{
    PS_STATIC_ASSERT(PS_WAL_REC_HEADER_SIZE == 16, "WAL record header is 16 bytes");
    T_EQ(ps_wal_rec_size(0), 16);
    T_EQ(ps_wal_rec_size(1), 24);
    T_EQ(ps_wal_rec_size(8), 24);
    T_EQ(ps_wal_rec_size(9), 32);
    T_EQ(ps_wal_rec_size(0xFFFFFFFFu), 16 + 0x100000000ull);
}

static void test_wal_rec_roundtrip_and_rejects(void)
{
    u8 b[64];
    u8 lsn_le[8];
    u64 lsn = 0;
    const u8 *payload = NULL;
    u32 len = 0;
    usize size = 0;

    memset(b, 0xCC, sizeof(b));
    ps_wal_rec_encode(b, 42, "hello", 5);
    T_EQ(ps_get_le32(b), 5);
    ps_put_le64(lsn_le, 42);
    T_EQ(ps_get_le32(b + 4), ps_crc32c(ps_crc32c(0, lsn_le, 8), "hello", 5));
    T_EQ(ps_get_le64(b + 8), 42);
    T_MEMEQ(b + 16, "hello", 5);
    T_MEMEQ(b + 21, "\0\0\0", 3);
    T_EQ(b[24], 0xCC); /* byte after the record */

    T_OK(ps_wal_rec_decode(b, sizeof(b), 41, &lsn, &payload, &len, &size));
    T_EQ(lsn, 42);
    T_EQ(len, 5);
    T_EQ(size, 24);
    T_CHECK(payload == b + 16);
    T_MEMEQ(payload, "hello", 5);

    T_OK(ps_wal_rec_decode(b, 24, 0, &lsn, &payload, &len, &size));

    T_ERR(ps_wal_rec_decode(b, sizeof(b), 42, &lsn, &payload, &len, &size), EBADMSG);
    T_ERR(ps_wal_rec_decode(b, sizeof(b), 100, &lsn, &payload, &len, &size), EBADMSG);

    T_ERR(ps_wal_rec_decode(b, 23, 41, &lsn, &payload, &len, &size), EBADMSG);
    T_ERR(ps_wal_rec_decode(b, 15, 41, &lsn, &payload, &len, &size), EBADMSG);
    T_ERR(ps_wal_rec_decode(b, 0, 41, &lsn, &payload, &len, &size), EBADMSG);

    for (int i = 0; i < 24; i++) {
        for (int bit = 0; bit < 8; bit++) {
            b[i] ^= (u8) (1u << bit);
            T_ERR(ps_wal_rec_decode(b, sizeof(b), 0, &lsn, &payload, &len, &size), EBADMSG);
            b[i] ^= (u8) (1u << bit);
        }
    }
    T_OK(ps_wal_rec_decode(b, sizeof(b), 0, &lsn, &payload, &len, &size));

    memset(b, 0, sizeof(b));
    T_ERR(ps_wal_rec_decode(b, sizeof(b), 0, &lsn, &payload, &len, &size), EBADMSG);

    ps_wal_rec_encode(b, 1, NULL, 0);
    T_OK(ps_wal_rec_decode(b, sizeof(b), 0, &lsn, &payload, &len, &size));
    T_EQ(lsn, 1);
    T_EQ(len, 0);
    T_EQ(size, 16);
    ps_wal_rec_encode(b, 0xFFFFFFFFFFFFFFFFull, "12345678", 8);
    T_OK(ps_wal_rec_decode(b, sizeof(b), 7, &lsn, &payload, &len, &size));
    T_EQ(lsn, 0xFFFFFFFFFFFFFFFFull);
    T_EQ(len, 8);
    T_EQ(size, 24);
    T_MEMEQ(payload, "12345678", 8);

    ps_wal_rec_encode(b, 10, "abc", 3);
    ps_wal_rec_encode(b + ps_wal_rec_size(3), 11, "defghijkl", 9);
    T_OK(ps_wal_rec_decode(b, sizeof(b), 9, &lsn, &payload, &len, &size));
    T_EQ(lsn, 10);
    T_OK(ps_wal_rec_decode(b + size, sizeof(b) - size, lsn, &lsn, &payload, &len, &size));
    T_EQ(lsn, 11);
    T_EQ(len, 9);
    T_EQ(size, 32);
    T_MEMEQ(payload, "defghijkl", 9);

    /* The claimed length overflows the padded size on a 32-bit host. */
    ps_put_le32(b, 0xFFFFFFF0u);
    T_ERR(ps_wal_rec_decode(b, sizeof(b), 0, &lsn, &payload, &len, &size), EBADMSG);
}

int main(int argc, char **argv)
{
    t_init(argc, argv);
    T_RUN(test_data_header_layout);
    T_RUN(test_data_header_roundtrip_and_corruption);
    T_RUN(test_journal_layout);
    T_RUN(test_wal_header_roundtrip);
    T_RUN(test_wal_rec_size);
    T_RUN(test_wal_rec_roundtrip_and_rejects);
    return t_done();
}
