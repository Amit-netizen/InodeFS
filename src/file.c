/*
 * file.c
 */

#include "litefs.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define PTRS_PER_BLOCK \
    (LITEFS_BLOCK_SIZE / sizeof(uint32_t))

static int get_block_ptr(
    struct litefs_state *fs,
    struct litefs_inode *inode,
    uint32_t logical,
    uint32_t *out_phys,
    int allocate
) {
    if (logical < LITEFS_DIRECT_BLOCKS) {
        if (!inode->i_direct[logical]) {
            if (!allocate) {
                *out_phys = 0;
                return 0;
            }

            uint32_t blk = 0;

            int ret = block_alloc(fs, &blk);

            if (ret < 0)
                return ret;

            inode->i_direct[logical] = blk;

            inode->i_blocks++;
        }

        *out_phys = inode->i_direct[logical];

        return 0;
    }

    logical -= LITEFS_DIRECT_BLOCKS;

    if (logical < PTRS_PER_BLOCK) {
        if (!inode->i_indirect) {
            if (!allocate) {
                *out_phys = 0;
                return 0;
            }

            int ret =
                block_alloc(
                    fs,
                    &inode->i_indirect
                );

            if (ret < 0)
                return ret;

            inode->i_blocks++;
        }

        uint32_t ptrs[PTRS_PER_BLOCK];

        block_read(
            fs,
            inode->i_indirect,
            ptrs
        );

        if (!ptrs[logical]) {
            if (!allocate) {
                *out_phys = 0;
                return 0;
            }

            int ret =
                block_alloc(
                    fs,
                    &ptrs[logical]
                );

            if (ret < 0)
                return ret;

            inode->i_blocks++;

            block_write(
                fs,
                inode->i_indirect,
                ptrs
            );
        }

        *out_phys = ptrs[logical];

        return 0;
    }

    logical -= PTRS_PER_BLOCK;

    if (
        logical <
        PTRS_PER_BLOCK * PTRS_PER_BLOCK
    ) {
        if (!inode->i_dindirect) {
            if (!allocate) {
                *out_phys = 0;
                return 0;
            }

            int ret =
                block_alloc(
                    fs,
                    &inode->i_dindirect
                );

            if (ret < 0)
                return ret;

            inode->i_blocks++;
        }

        uint32_t l1[PTRS_PER_BLOCK];

        block_read(
            fs,
            inode->i_dindirect,
            l1
        );

        uint32_t l1_idx =
            logical / PTRS_PER_BLOCK;

        uint32_t l2_idx =
            logical % PTRS_PER_BLOCK;

        if (!l1[l1_idx]) {
            if (!allocate) {
                *out_phys = 0;
                return 0;
            }

            int ret =
                block_alloc(
                    fs,
                    &l1[l1_idx]
                );

            if (ret < 0)
                return ret;

            inode->i_blocks++;

            block_write(
                fs,
                inode->i_dindirect,
                l1
            );
        }

        uint32_t l2[PTRS_PER_BLOCK];

        block_read(
            fs,
            l1[l1_idx],
            l2
        );

        if (!l2[l2_idx]) {
            if (!allocate) {
                *out_phys = 0;
                return 0;
            }

            int ret =
                block_alloc(
                    fs,
                    &l2[l2_idx]
                );

            if (ret < 0)
                return ret;

            inode->i_blocks++;

            block_write(
                fs,
                l1[l1_idx],
                l2
            );
        }

        *out_phys = l2[l2_idx];

        return 0;
    }

    return -EFBIG;
}

ssize_t file_read(
    struct litefs_state *fs,
    struct litefs_inode *inode,
    void *buf,
    size_t size,
    off_t offset
) {
    if (offset >= (off_t)inode->i_size)
        return 0;

    if (
        (off_t)(offset + size) >
        (off_t)inode->i_size
    ) {
        size = inode->i_size - offset;
    }

    size_t done = 0;

    uint8_t block_buf[LITEFS_BLOCK_SIZE];

    while (done < size) {
        uint32_t logical =
            (offset + done) /
            LITEFS_BLOCK_SIZE;

        uint32_t blk_off =
            (offset + done) %
            LITEFS_BLOCK_SIZE;

        uint32_t chunk =
            LITEFS_BLOCK_SIZE - blk_off;

        if (chunk > size - done)
            chunk = size - done;

        uint32_t phys = 0;

        int ret = get_block_ptr(
            fs,
            inode,
            logical,
            &phys,
            0
        );

        if (ret < 0)
            return ret;

        if (!phys) {
            memset(
                (uint8_t *)buf + done,
                0,
                chunk
            );
        } else {
            ret = block_read(
                fs,
                phys,
                block_buf
            );

            if (ret < 0)
                return ret;

            memcpy(
                (uint8_t *)buf + done,
                block_buf + blk_off,
                chunk
            );
        }

        done += chunk;
    }

    inode->i_atime = litefs_now();

    return (ssize_t)done;
}

ssize_t file_write(
    struct litefs_state *fs,
    struct litefs_inode *inode,
    const void *buf,
    size_t size,
    off_t offset
) {
    size_t done = 0;

    uint8_t block_buf[LITEFS_BLOCK_SIZE];

    journal_begin_txn(fs);

    while (done < size) {
        uint32_t logical =
            (offset + done) /
            LITEFS_BLOCK_SIZE;

        uint32_t blk_off =
            (offset + done) %
            LITEFS_BLOCK_SIZE;

        uint32_t chunk =
            LITEFS_BLOCK_SIZE - blk_off;

        if (chunk > size - done)
            chunk = size - done;

        uint32_t phys = 0;

        int ret = get_block_ptr(
            fs,
            inode,
            logical,
            &phys,
            1
        );

        if (ret < 0) {
            journal_commit_txn(fs);

            return ret;
        }

        if (
            blk_off != 0 ||
            chunk != LITEFS_BLOCK_SIZE
        ) {
            ret = block_read(
                fs,
                phys,
                block_buf
            );

            if (ret < 0) {
                journal_commit_txn(fs);

                return ret;
            }
        } else {
            memset(
                block_buf,
                0,
                LITEFS_BLOCK_SIZE
            );
        }

        memcpy(
            block_buf + blk_off,
            (const uint8_t *)buf + done,
            chunk
        );

        ret = block_write(
            fs,
            phys,
            block_buf
        );

        if (ret < 0) {
            journal_commit_txn(fs);

            return ret;
        }

        done += chunk;
    }

    if (
        (off_t)(offset + done) >
        (off_t)inode->i_size
    ) {
        inode->i_size = offset + done;
    }

    inode->i_mtime =
    inode->i_ctime =
        litefs_now();

    inode_flush(fs, inode->i_ino);

    journal_commit_txn(fs);

    return (ssize_t)done;
}

int file_truncate(
    struct litefs_state *fs,
    struct litefs_inode *inode,
    off_t new_size
) {
    if (new_size < 0)
        return -EINVAL;

    journal_begin_txn(fs);

    if ((uint64_t)new_size < inode->i_size) {
        uint32_t first_free_logical =
            (
                new_size +
                LITEFS_BLOCK_SIZE - 1
            ) / LITEFS_BLOCK_SIZE;

        uint32_t last_logical =
            (
                inode->i_size +
                LITEFS_BLOCK_SIZE - 1
            ) / LITEFS_BLOCK_SIZE;

        for (
            uint32_t l = first_free_logical;
            l < last_logical;
            l++
        ) {
            uint32_t phys = 0;

            get_block_ptr(
                fs,
                inode,
                l,
                &phys,
                0
            );

            if (phys)
                block_free(fs, phys);
        }
    }

    inode->i_size = new_size;

    inode->i_mtime =
    inode->i_ctime =
        litefs_now();

    inode_flush(fs, inode->i_ino);

    journal_commit_txn(fs);

    return 0;
}