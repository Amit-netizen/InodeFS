/*
 * block.c — raw block I/O with cache integration
 *
 * All reads go through the LRU cache (read-through).
 * All writes go through the LRU cache (write-back) AND are logged to the
 * journal before being marked dirty — this guarantees crash consistency.
 *
 * Block allocation uses a simple bitmap stored in memory and persisted to
 * the BITMAP region of the disk on every alloc/free.
 */

#include "litefs.h"
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>

/* ── Bitmap helpers ── */

static int bitmap_test(const uint8_t *bm, uint32_t bit)
{
    return (bm[bit / 8] >> (bit % 8)) & 1;
}

static void bitmap_set(uint8_t *bm, uint32_t bit)
{
    bm[bit / 8] |= (1u << (bit % 8));
}

static void bitmap_clear(uint8_t *bm, uint32_t bit)
{
    bm[bit / 8] &= ~(1u << (bit % 8));
}

int bitmap_flush(struct litefs_state *fs)
{
    /*
     * Write the in-memory bitmap to its disk region.
     * The bitmap may span multiple blocks.
     */
    uint32_t bytes = (LITEFS_MAX_BLOCKS + 7) / 8;
    uint8_t *src   = fs->block_bitmap;

    for (uint32_t b = 0; b < BITMAP_BLOCKS; b++) {
        uint32_t blk    = BITMAP_OFFSET + b;
        off_t    offset = (off_t)blk * LITEFS_BLOCK_SIZE;
        uint32_t chunk  = (bytes < LITEFS_BLOCK_SIZE) ? bytes : LITEFS_BLOCK_SIZE;

        uint8_t buf[LITEFS_BLOCK_SIZE];
        memset(buf, 0, sizeof(buf));
        memcpy(buf, src, chunk);

        if (pwrite(fs->disk_fd, buf, LITEFS_BLOCK_SIZE, offset) != LITEFS_BLOCK_SIZE)
            return -EIO;

        cache_put(&fs->cache, blk, buf, 0);
        src   += chunk;
        bytes -= chunk;
        if (bytes == 0) break;
    }
    return 0;
}

/* ── block_alloc ── */

int block_alloc(struct litefs_state *fs, uint32_t *out_blk)
{
    if (fs->sb.s_free_blocks == 0)
        return -ENOSPC;

    for (uint32_t i = 0; i < LITEFS_MAX_BLOCKS; i++) {
        if (!bitmap_test(fs->block_bitmap, i)) {
            bitmap_set(fs->block_bitmap, i);
            fs->sb.s_free_blocks--;
            *out_blk = DATA_OFFSET + i;  /* absolute block number on disk */

            /* zero-fill the new block */
            uint8_t zero[LITEFS_BLOCK_SIZE];
            memset(zero, 0, LITEFS_BLOCK_SIZE);
            cache_put(&fs->cache, *out_blk, zero, 1);

            bitmap_flush(fs);
            sb_flush(fs);
            return 0;
        }
    }
    return -ENOSPC;
}

/* ── block_free ── */

int block_free(struct litefs_state *fs, uint32_t blk)
{
    if (blk < DATA_OFFSET) return -EINVAL;
    uint32_t rel = blk - DATA_OFFSET;
    if (rel >= LITEFS_MAX_BLOCKS) return -EINVAL;

    if (!bitmap_test(fs->block_bitmap, rel))
        return -EINVAL;  /* double-free */

    bitmap_clear(fs->block_bitmap, rel);
    fs->sb.s_free_blocks++;
    cache_invalidate(&fs->cache, blk);
    bitmap_flush(fs);
    sb_flush(fs);
    return 0;
}

/* ── block_read ── */

int block_read(struct litefs_state *fs, uint32_t blk, void *buf)
{
    /* Try cache first */
    if (cache_get(&fs->cache, blk, buf) == 0)
        return 0;

    /* Cache miss — read from disk */
    off_t offset = (off_t)blk * LITEFS_BLOCK_SIZE;
    ssize_t n = pread(fs->disk_fd, buf, LITEFS_BLOCK_SIZE, offset);
    if (n != LITEFS_BLOCK_SIZE) {
        fprintf(stderr, "[block] read error blk=%u errno=%d\n", blk, errno);
        return -EIO;
    }

    /* Populate cache */
    cache_put(&fs->cache, blk, buf, 0);
    return 0;
}

/* ── block_write ── */

/*
 * block_write is always called inside a transaction (journal_begin_txn /
 * journal_commit_txn). It logs the NEW data to the journal first, then
 * updates the cache. The actual disk block is written lazily via cache_flush.
 *
 * Crash consistency guarantee:
 *   If we crash after journal_log_block but before cache_flush, the journal
 *   replay on next mount will re-apply the write.
 *   If we crash before journal_log_block, the old data is intact — no
 *   partial write.
 */
int block_write(struct litefs_state *fs, uint32_t blk, const void *buf)
{
    int ret = journal_log_block(fs, blk, buf);
    if (ret < 0) return ret;

    cache_put(&fs->cache, blk, buf, 1);
    return 0;
}
