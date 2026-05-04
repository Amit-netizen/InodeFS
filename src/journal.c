/*
 * journal.c — Write-Ahead Log (WAL) for crash consistency
 *
 * Design (ext3/ext4 ordered-mode inspired):
 *
 *   A transaction is:
 *     1. BEGIN  (implicit — journal_begin_txn sets cur_txn_seq)
 *     2. LOG    — for every block_write, copy the new block data into the
 *                 journal with a JOURNAL_BLOCK_DATA record
 *     3. COMMIT — write a JOURNAL_BLOCK_COMMIT record; now the transaction
 *                 is durable
 *     4. CHECKPOINT — after cache_flush has pushed all dirty blocks to their
 *                 real disk locations, advance j_tail past this transaction
 *
 * Recovery on mount:
 *   Read journal from j_tail to j_head; for each complete transaction
 *   (i.e. a DATA sequence followed by a COMMIT record with matching seq),
 *   re-apply the blocks to disk. Incomplete transactions are discarded.
 *
 * The journal is a separate file (*.journal) treated as a circular buffer
 * of JOURNAL_SIZE_BLOCKS blocks (each LITEFS_BLOCK_SIZE bytes).
 */

#include "litefs.h"
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>

/* ── CRC32 (simple polynomial) ── */

uint32_t crc32_simple(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return crc ^ 0xFFFFFFFF;
}

/* ── Low-level journal block I/O ── */

static int jblk_write(struct litefs_state *fs, uint32_t jidx, const void *buf)
{
    off_t off = (off_t)(1 + jidx) * LITEFS_BLOCK_SIZE; /* block 0 = journal header */
    if (pwrite(fs->journal_fd, buf, LITEFS_BLOCK_SIZE, off) != LITEFS_BLOCK_SIZE)
        return -EIO;
    return 0;
}

static int jblk_read(struct litefs_state *fs, uint32_t jidx, void *buf)
{
    off_t off = (off_t)(1 + jidx) * LITEFS_BLOCK_SIZE;
    if (pread(fs->journal_fd, buf, LITEFS_BLOCK_SIZE, off) != LITEFS_BLOCK_SIZE)
        return -EIO;
    return 0;
}

static int jheader_flush(struct litefs_state *fs)
{
    if (pwrite(fs->journal_fd, &fs->jh, sizeof(fs->jh), 0) != sizeof(fs->jh))
        return -EIO;
    return 0;
}

/* ── journal_begin_txn ── */

int journal_begin_txn(struct litefs_state *fs)
{
    fs->cur_txn_seq = ++fs->jh.j_sequence;
    /* Mark FS dirty in superblock so we know to replay on next mount */
    fs->sb.s_state = 1;
    sb_flush(fs);
    return 0;
}

/* ── journal_log_block ── */

/*
 * Write one block's worth of data into the circular journal log.
 * Layout of each journal slot:
 *   [ struct journal_block_header (32 bytes) | data (LITEFS_BLOCK_SIZE - 32 bytes) ]
 * The header and data are packed into one LITEFS_BLOCK_SIZE page.
 */
int journal_log_block(struct litefs_state *fs, uint32_t block_no, const void *data)
{
    /*
     * Each logged block occupies TWO journal slots:
     *   Slot N+0: journal_block_header (descriptor record)
     *   Slot N+1: the full LITEFS_BLOCK_SIZE bytes of block data
     *
     * This avoids the truncation bug where packing header + data into one
     * block loses sizeof(header) bytes from the end of every stored block.
     */
    uint8_t hdr_buf[LITEFS_BLOCK_SIZE];
    memset(hdr_buf, 0, sizeof(hdr_buf));
    struct journal_block_header *hdr = (struct journal_block_header *)hdr_buf;

    hdr->jb_magic        = JOURNAL_MAGIC;
    hdr->jb_type         = JOURNAL_BLOCK_DATA;
    hdr->jb_sequence     = fs->cur_txn_seq;
    hdr->jb_target_block = block_no;
    hdr->jb_checksum     = crc32_simple(data, LITEFS_BLOCK_SIZE);

    /* Write descriptor block */
    uint32_t jidx = fs->jh.j_head % JOURNAL_SIZE_BLOCKS;
    int ret = jblk_write(fs, jidx, hdr_buf);
    if (ret < 0) return ret;
    fs->jh.j_head = (fs->jh.j_head + 1) % JOURNAL_SIZE_BLOCKS;

    /* Write raw data block */
    jidx = fs->jh.j_head % JOURNAL_SIZE_BLOCKS;
    ret = jblk_write(fs, jidx, data);
    if (ret < 0) return ret;
    fs->jh.j_head = (fs->jh.j_head + 1) % JOURNAL_SIZE_BLOCKS;

    return jheader_flush(fs);
}

/* ── journal_commit_txn ── */

int journal_commit_txn(struct litefs_state *fs)
{
    uint8_t jbuf[LITEFS_BLOCK_SIZE];
    struct journal_block_header *hdr = (struct journal_block_header *)jbuf;
    memset(jbuf, 0, sizeof(jbuf));

    hdr->jb_magic    = JOURNAL_MAGIC;
    hdr->jb_type     = JOURNAL_BLOCK_COMMIT;
    hdr->jb_sequence = fs->cur_txn_seq;
    hdr->jb_checksum = crc32_simple(jbuf + sizeof(*hdr),
                                     LITEFS_BLOCK_SIZE - sizeof(*hdr));

    uint32_t jidx = fs->jh.j_head % JOURNAL_SIZE_BLOCKS;
    int ret = jblk_write(fs, jidx, jbuf);
    if (ret < 0) return ret;

    fs->jh.j_head = (fs->jh.j_head + 1) % JOURNAL_SIZE_BLOCKS;

    /* fdatasync to ensure commit record hits persistent storage */
    fdatasync(fs->journal_fd);

    return jheader_flush(fs);
}

/* ── journal_replay ── */

/*
 * Called at mount time if sb.s_state == 1 (filesystem was not cleanly unmounted).
 *
 * Algorithm:
 *   1. Scan journal from j_tail to j_head
 *   2. Collect DATA blocks per transaction sequence number
 *   3. When we see a COMMIT record for seq S, apply all DATA blocks for S to disk
 *   4. If we reach j_head without a COMMIT for a transaction, discard it (incomplete)
 */
int journal_replay(struct litefs_state *fs)
{
    printf("[journal] replaying from tail=%u head=%u\n",
           fs->jh.j_tail, fs->jh.j_head);

    if (fs->jh.j_tail == fs->jh.j_head) {
        printf("[journal] journal empty, nothing to replay\n");
        return 0;
    }

    /* We'll collect pending DATA records keyed by (seq, target_block) */
#define MAX_PENDING 512
    struct {
        uint32_t seq;
        uint32_t target_block;
        uint8_t  data[LITEFS_BLOCK_SIZE];   /* full block — two-slot layout */
    } pending[MAX_PENDING];
    int npending = 0;

    uint32_t pos = fs->jh.j_tail;

    while (pos != fs->jh.j_head) {
        uint8_t jbuf[LITEFS_BLOCK_SIZE];
        uint32_t jidx = pos % JOURNAL_SIZE_BLOCKS;

        if (jblk_read(fs, jidx, jbuf) < 0) {
            fprintf(stderr, "[journal] replay read error at jidx=%u\n", jidx);
            break;
        }

        struct journal_block_header *hdr = (struct journal_block_header *)jbuf;

        if (hdr->jb_magic != JOURNAL_MAGIC) {
            /* Corrupt or unwritten slot — stop */
            fprintf(stderr, "[journal] bad magic at jidx=%u, stopping replay\n", jidx);
            break;
        }

        if (hdr->jb_type == JOURNAL_BLOCK_DATA) {
            if (npending < MAX_PENDING) {
                pending[npending].seq          = hdr->jb_sequence;
                pending[npending].target_block = hdr->jb_target_block;

                /* Next slot contains the full raw block data */
                uint32_t data_jidx = (pos + 1) % JOURNAL_SIZE_BLOCKS;
                if (jblk_read(fs, data_jidx, pending[npending].data) < 0) {
                    fprintf(stderr, "[journal] replay: failed to read data slot at jidx=%u\n",
                            data_jidx);
                } else {
                    /* Verify checksum */
                    uint32_t expected_crc = hdr->jb_checksum;
                    uint32_t actual_crc   = crc32_simple(pending[npending].data,
                                                          LITEFS_BLOCK_SIZE);
                    if (expected_crc != actual_crc)
                        fprintf(stderr, "[journal] replay: CRC mismatch blk=%u "
                                "(expected 0x%X got 0x%X)\n",
                                hdr->jb_target_block, expected_crc, actual_crc);
                }
                npending++;
            }
            /* Skip the data slot — we already consumed it above */
            pos = (pos + 1) % JOURNAL_SIZE_BLOCKS;

        } else if (hdr->jb_type == JOURNAL_BLOCK_COMMIT) {
            uint32_t committed_seq = hdr->jb_sequence;

            /* Apply all pending DATA blocks for this sequence */
            for (int i = 0; i < npending; i++) {
                if (pending[i].seq != committed_seq) continue;

                uint32_t tblk = pending[i].target_block;
                off_t    off  = (off_t)tblk * LITEFS_BLOCK_SIZE;

                if (pwrite(fs->disk_fd, pending[i].data, LITEFS_BLOCK_SIZE, off)
                        != LITEFS_BLOCK_SIZE) {
                    fprintf(stderr, "[journal] replay write error blk=%u\n", tblk);
                }
                cache_invalidate(&fs->cache, tblk);
                printf("[journal]   replayed block %u (seq=%u)\n", tblk, committed_seq);
            }

            /* Remove committed entries from pending list */
            int kept = 0;
            for (int i = 0; i < npending; i++)
                if (pending[i].seq != committed_seq)
                    pending[kept++] = pending[i];
            npending = kept;
        }

        pos = (pos + 1) % JOURNAL_SIZE_BLOCKS;
    }

    if (npending > 0)
        printf("[journal] discarding %d uncommitted records\n", npending);

    /* Advance tail to head — journal is now clean */
    fs->jh.j_tail = fs->jh.j_head;
    jheader_flush(fs);

    /* Mark FS clean */
    fs->sb.s_state = 0;
    sb_flush(fs);

    printf("[journal] replay complete\n");
    return 0;
}

/* ── journal_checkpoint ── */

/*
 * After all dirty blocks have been flushed to their real disk locations
 * (cache_flush), we can advance j_tail to j_head, freeing journal space.
 */
void journal_checkpoint(struct litefs_state *fs)
{
    cache_flush(&fs->cache, fs->disk_fd);
    fdatasync(fs->disk_fd);

    fs->jh.j_tail = fs->jh.j_head;
    jheader_flush(fs);

    fs->sb.s_state = 0;
    sb_flush(fs);

    printf("[journal] checkpoint complete (tail=head=%u)\n", fs->jh.j_head);
}
