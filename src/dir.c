/*
 * dir.c
 */

#include "litefs.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define DIRENTS_PER_BLOCK \
    (LITEFS_BLOCK_SIZE / sizeof(struct litefs_dirent))

int dir_lookup(
    struct litefs_state *fs,
    uint32_t dir_ino,
    const char *name,
    uint32_t *out_ino
) {
    struct litefs_inode *dir =
        inode_get(fs, dir_ino);

    if (!dir || dir->i_type != LITEFS_INODE_DIR)
        return -ENOTDIR;

    uint8_t buf[LITEFS_BLOCK_SIZE];

    for (int b = 0; b < LITEFS_DIRECT_BLOCKS; b++) {
        if (!dir->i_direct[b])
            continue;

        if (
            block_read(
                fs,
                dir->i_direct[b],
                buf
            ) < 0
        ) {
            return -EIO;
        }

        struct litefs_dirent *entries =
            (struct litefs_dirent *)buf;

        for (
            uint32_t i = 0;
            i < DIRENTS_PER_BLOCK;
            i++
        ) {
            if (!entries[i].d_ino)
                continue;

            if (
                strncmp(
                    entries[i].d_name,
                    name,
                    LITEFS_NAME_MAX
                ) == 0
            ) {
                *out_ino = entries[i].d_ino;

                return 0;
            }
        }
    }

    return -ENOENT;
}

int dir_add_entry(
    struct litefs_state *fs,
    uint32_t dir_ino,
    const char *name,
    uint32_t ino,
    uint8_t type
) {
    struct litefs_inode *dir =
        inode_get(fs, dir_ino);

    if (!dir || dir->i_type != LITEFS_INODE_DIR)
        return -ENOTDIR;

    uint8_t buf[LITEFS_BLOCK_SIZE];

    for (int b = 0; b < LITEFS_DIRECT_BLOCKS; b++) {
        if (!dir->i_direct[b])
            continue;

        if (
            block_read(
                fs,
                dir->i_direct[b],
                buf
            ) < 0
        ) {
            return -EIO;
        }

        struct litefs_dirent *entries =
            (struct litefs_dirent *)buf;

        for (
            uint32_t i = 0;
            i < DIRENTS_PER_BLOCK;
            i++
        ) {
            if (entries[i].d_ino != 0)
                continue;

            entries[i].d_ino = ino;

            entries[i].d_type = type;

            entries[i].d_namelen =
                (uint8_t)strlen(name);

            strncpy(
                entries[i].d_name,
                name,
                LITEFS_NAME_MAX
            );

            entries[i].d_name[LITEFS_NAME_MAX] =
                '\0';

            journal_begin_txn(fs);

            block_write(
                fs,
                dir->i_direct[b],
                buf
            );

            uint64_t needed =
                (uint64_t)(b + 1) *
                LITEFS_BLOCK_SIZE;

            if (dir->i_size < needed)
                dir->i_size = needed;

            dir->i_mtime =
            dir->i_ctime =
                litefs_now();

            inode_flush(fs, dir_ino);

            journal_commit_txn(fs);

            return 0;
        }
    }

    for (int b = 0; b < LITEFS_DIRECT_BLOCKS; b++) {
        if (dir->i_direct[b])
            continue;

        uint32_t new_blk = 0;

        int ret = block_alloc(fs, &new_blk);

        if (ret < 0)
            return ret;

        dir->i_direct[b] = new_blk;

        dir->i_blocks++;

        memset(buf, 0, LITEFS_BLOCK_SIZE);

        struct litefs_dirent *entries =
            (struct litefs_dirent *)buf;

        entries[0].d_ino = ino;

        entries[0].d_type = type;

        entries[0].d_namelen =
            (uint8_t)strlen(name);

        strncpy(
            entries[0].d_name,
            name,
            LITEFS_NAME_MAX
        );

        entries[0].d_name[LITEFS_NAME_MAX] =
            '\0';

        journal_begin_txn(fs);

        block_write(
            fs,
            dir->i_direct[b],
            buf
        );

        dir->i_size += LITEFS_BLOCK_SIZE;

        dir->i_mtime =
        dir->i_ctime =
            litefs_now();

        inode_flush(fs, dir_ino);

        journal_commit_txn(fs);

        return 0;
    }

    return -ENOSPC;
}

int dir_remove_entry(
    struct litefs_state *fs,
    uint32_t dir_ino,
    const char *name
) {
    struct litefs_inode *dir =
        inode_get(fs, dir_ino);

    if (!dir || dir->i_type != LITEFS_INODE_DIR)
        return -ENOTDIR;

    uint8_t buf[LITEFS_BLOCK_SIZE];

    for (int b = 0; b < LITEFS_DIRECT_BLOCKS; b++) {
        if (!dir->i_direct[b])
            continue;

        if (
            block_read(
                fs,
                dir->i_direct[b],
                buf
            ) < 0
        ) {
            return -EIO;
        }

        struct litefs_dirent *entries =
            (struct litefs_dirent *)buf;

        for (
            uint32_t i = 0;
            i < DIRENTS_PER_BLOCK;
            i++
        ) {
            if (!entries[i].d_ino)
                continue;

            if (
                strncmp(
                    entries[i].d_name,
                    name,
                    LITEFS_NAME_MAX
                ) == 0
            ) {
                memset(
                    &entries[i],
                    0,
                    sizeof(entries[i])
                );

                journal_begin_txn(fs);

                block_write(
                    fs,
                    dir->i_direct[b],
                    buf
                );

                dir->i_mtime =
                dir->i_ctime =
                    litefs_now();

                inode_flush(fs, dir_ino);

                journal_commit_txn(fs);

                return 0;
            }
        }
    }

    return -ENOENT;
}

int dir_readdir(
    struct litefs_state *fs,
    uint32_t dir_ino,
    void *buf,
    fuse_fill_dir_t filler
) {
    struct litefs_inode *dir =
        inode_get(fs, dir_ino);

    if (!dir || dir->i_type != LITEFS_INODE_DIR)
        return -ENOTDIR;

    uint8_t blk_buf[LITEFS_BLOCK_SIZE];

    for (int b = 0; b < LITEFS_DIRECT_BLOCKS; b++) {
        if (!dir->i_direct[b])
            continue;

        if (
            block_read(
                fs,
                dir->i_direct[b],
                blk_buf
            ) < 0
        ) {
            continue;
        }

        struct litefs_dirent *entries =
            (struct litefs_dirent *)blk_buf;

        for (
            uint32_t i = 0;
            i < DIRENTS_PER_BLOCK;
            i++
        ) {
            if (!entries[i].d_ino)
                continue;

            filler(
                buf,
                entries[i].d_name,
                NULL,
                0,
                0
            );
        }
    }

    return 0;
}