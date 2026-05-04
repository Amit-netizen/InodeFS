/*
 * inode.c — inode allocation, lookup, and persistence
 *
 * The inode table is loaded entirely into memory at mount time
 * (fs->inode_table[]). Changes are written back to the INODE_TABLE region
 * of the disk via inode_flush(). All inode mutations go through a journal
 * transaction.
 */

#include "litefs.h"
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>

uint64_t litefs_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec;
}

/* ── inode_get ── */

struct litefs_inode *inode_get(struct litefs_state *fs, uint32_t ino)
{
    if (ino == 0 || ino >= LITEFS_MAX_INODES)
        return NULL;
    if (fs->inode_table[ino].i_type == LITEFS_INODE_FREE)
        return NULL;
    return &fs->inode_table[ino];
}

/* ── inode_alloc ── */

int inode_alloc(struct litefs_state *fs, uint8_t type, uint16_t mode, uint32_t *out_ino)
{
    if (fs->sb.s_free_inodes == 0)
        return -ENOSPC;

    for (uint32_t i = 1; i < LITEFS_MAX_INODES; i++) {
        if (fs->inode_table[i].i_type == LITEFS_INODE_FREE) {
            struct litefs_inode *inode = &fs->inode_table[i];
            memset(inode, 0, sizeof(*inode));
            inode->i_ino   = i;
            inode->i_type  = type;
            inode->i_mode  = mode;
            inode->i_uid   = getuid();
            inode->i_gid   = getgid();
            inode->i_links = 1;
            inode->i_atime = inode->i_mtime = inode->i_ctime = litefs_now();

            fs->sb.s_free_inodes--;
            *out_ino = i;

            inode_flush(fs, i);
            sb_flush(fs);
            return 0;
        }
    }
    return -ENOSPC;
}

/* ── inode_free ── */

int inode_free(struct litefs_state *fs, uint32_t ino)
{
    struct litefs_inode *inode = inode_get(fs, ino);
    if (!inode) return -ENOENT;

    /* Free all data blocks */
    for (int i = 0; i < LITEFS_DIRECT_BLOCKS; i++) {
        if (inode->i_direct[i])
            block_free(fs, inode->i_direct[i]);
    }

    /* TODO: free indirect and double-indirect blocks for large files */
    if (inode->i_indirect) {
        uint32_t ptrs[LITEFS_BLOCK_SIZE / sizeof(uint32_t)];
        block_read(fs, inode->i_indirect, ptrs);
        for (size_t i = 0; i < LITEFS_BLOCK_SIZE / sizeof(uint32_t); i++)
            if (ptrs[i]) block_free(fs, ptrs[i]);
        block_free(fs, inode->i_indirect);
    }

    memset(inode, 0, sizeof(*inode));
    inode->i_type = LITEFS_INODE_FREE;
    fs->sb.s_free_inodes++;

    inode_flush(fs, ino);
    sb_flush(fs);
    return 0;
}

/* ── inode_flush ── */

/*
 * Serialize this inode back to its slot in the INODE_TABLE disk region.
 * We compute which block holds this inode, read it, update the slot,
 * then write it back — journaled.
 */
int inode_flush(struct litefs_state *fs, uint32_t ino)
{
    uint32_t inodes_per_block = LITEFS_BLOCK_SIZE / sizeof(struct litefs_inode);
    uint32_t block_idx = ino / inodes_per_block;
    uint32_t slot      = ino % inodes_per_block;
    uint32_t blk       = INODE_TABLE_OFFSET + block_idx;

    uint8_t buf[LITEFS_BLOCK_SIZE];
    if (block_read(fs, blk, buf) < 0) return -EIO;

    struct litefs_inode *table = (struct litefs_inode *)buf;
    memcpy(&table[slot], &fs->inode_table[ino], sizeof(struct litefs_inode));

    return block_write(fs, blk, buf);
}
