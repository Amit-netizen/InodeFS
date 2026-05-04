/*
 * fs.c
 */

#include "litefs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int sb_flush(struct litefs_state *fs) {
    fs->sb.s_write_time = litefs_now();

    if (
        pwrite(
            fs->disk_fd,
            &fs->sb,
            sizeof(fs->sb),
            0
        ) != sizeof(fs->sb)
    ) {
        return -EIO;
    }

    return 0;
}

int litefs_init_disk(
    const char *disk_path,
    const char *journal_path,
    uint32_t total_blocks
) {
    int fd = open(
        disk_path,
        O_RDWR | O_CREAT | O_TRUNC,
        0644
    );

    if (fd < 0) {
        perror("open disk");

        return -errno;
    }

    off_t disk_size =
        (off_t)(DATA_OFFSET + total_blocks) *
        LITEFS_BLOCK_SIZE;

    if (ftruncate(fd, disk_size) < 0) {
        perror("ftruncate");

        close(fd);

        return -errno;
    }

    struct litefs_superblock sb;

    memset(&sb, 0, sizeof(sb));

    sb.s_magic = LITEFS_MAGIC;

    sb.s_version = LITEFS_VERSION;

    sb.s_block_size = LITEFS_BLOCK_SIZE;

    sb.s_total_blocks = total_blocks;

    sb.s_free_blocks = total_blocks;

    sb.s_total_inodes = LITEFS_MAX_INODES;

    sb.s_free_inodes =
        LITEFS_MAX_INODES - 2;

    sb.s_root_ino = LITEFS_ROOT_INO;

    sb.s_mount_time = litefs_now();

    sb.s_state = 0;

    pwrite(fd, &sb, sizeof(sb), 0);

    struct litefs_inode root_inode;

    memset(
        &root_inode,
        0,
        sizeof(root_inode)
    );

    root_inode.i_ino = LITEFS_ROOT_INO;

    root_inode.i_type = LITEFS_INODE_DIR;

    root_inode.i_mode = 0755;

    root_inode.i_links = 2;

    root_inode.i_atime =
    root_inode.i_mtime =
    root_inode.i_ctime =
        litefs_now();

    off_t inode_off =
        (off_t)INODE_TABLE_OFFSET *
        LITEFS_BLOCK_SIZE +
        (off_t)LITEFS_ROOT_INO *
        sizeof(struct litefs_inode);

    pwrite(
        fd,
        &root_inode,
        sizeof(root_inode),
        inode_off
    );

    close(fd);

    int jfd = open(
        journal_path,
        O_RDWR | O_CREAT | O_TRUNC,
        0644
    );

    if (jfd < 0) {
        perror("open journal");

        return -errno;
    }

    off_t j_size =
        (off_t)(1 + JOURNAL_SIZE_BLOCKS) *
        LITEFS_BLOCK_SIZE;

    if (ftruncate(jfd, j_size) < 0) {
        perror("ftruncate journal");

        close(jfd);

        return -errno;
    }

    struct journal_header jh;

    memset(&jh, 0, sizeof(jh));

    jh.j_magic = JOURNAL_MAGIC;

    jh.j_version = LITEFS_VERSION;

    jh.j_size = JOURNAL_SIZE_BLOCKS;

    pwrite(jfd, &jh, sizeof(jh), 0);

    close(jfd);

    printf(
        "[mkfs] LiteFS disk: %s  "
        "journal: %s  blocks: %u\n",
        disk_path,
        journal_path,
        total_blocks
    );

    return 0;
}

int litefs_mount(
    const char *disk_path,
    const char *journal_path,
    struct litefs_state **out
) {
    struct litefs_state *fs =
        calloc(1, sizeof(*fs));

    if (!fs)
        return -ENOMEM;

    fs->disk_path = strdup(disk_path);

    fs->journal_path = strdup(journal_path);

    fs->disk_fd = open(
        disk_path,
        O_RDWR
    );

    if (fs->disk_fd < 0) {
        perror("open disk");

        free(fs);

        return -errno;
    }

    fs->journal_fd = open(
        journal_path,
        O_RDWR
    );

    if (fs->journal_fd < 0) {
        perror("open journal");

        close(fs->disk_fd);

        free(fs);

        return -errno;
    }

    if (
        pread(
            fs->disk_fd,
            &fs->sb,
            sizeof(fs->sb),
            0
        ) != sizeof(fs->sb)
    ) {
        fprintf(
            stderr,
            "Failed to read superblock\n"
        );

        goto err;
    }

    if (fs->sb.s_magic != LITEFS_MAGIC) {
        fprintf(
            stderr,
            "Bad magic: expected "
            "0x%X got 0x%X\n",
            LITEFS_MAGIC,
            fs->sb.s_magic
        );

        goto err;
    }

    off_t inode_region =
        (off_t)INODE_TABLE_OFFSET *
        LITEFS_BLOCK_SIZE;

    if (
        pread(
            fs->disk_fd,
            fs->inode_table,
            sizeof(fs->inode_table),
            inode_region
        ) !=
        (ssize_t)sizeof(fs->inode_table)
    ) {
        fprintf(
            stderr,
            "Failed to read inode table\n"
        );

        goto err;
    }

    off_t bitmap_region =
        (off_t)BITMAP_OFFSET *
        LITEFS_BLOCK_SIZE;

    uint32_t bm_bytes =
        (LITEFS_MAX_BLOCKS + 7) / 8;

    if (
        pread(
            fs->disk_fd,
            fs->block_bitmap,
            bm_bytes,
            bitmap_region
        ) != (ssize_t)bm_bytes
    ) {
        fprintf(
            stderr,
            "Failed to read block bitmap\n"
        );

        goto err;
    }

    if (
        pread(
            fs->journal_fd,
            &fs->jh,
            sizeof(fs->jh),
            0
        ) != sizeof(fs->jh)
    ) {
        fprintf(
            stderr,
            "Failed to read journal header\n"
        );

        goto err;
    }

    cache_init(&fs->cache);

    pthread_mutex_init(
        &fs->fs_lock,
        NULL
    );

    if (fs->sb.s_state != 0) {
        printf(
            "[mount] filesystem was "
            "not cleanly unmounted, "
            "replaying journal...\n"
        );

        journal_replay(fs);

        pread(
            fs->disk_fd,
            fs->inode_table,
            sizeof(fs->inode_table),
            inode_region
        );

        pread(
            fs->disk_fd,
            fs->block_bitmap,
            bm_bytes,
            bitmap_region
        );
    }

    fs->sb.s_mount_time =
        litefs_now();

    fs->sb.s_state = 1;

    sb_flush(fs);

    printf(
        "[mount] LiteFS mounted: "
        "%u free blocks, "
        "%u free inodes\n",
        fs->sb.s_free_blocks,
        fs->sb.s_free_inodes
    );

    *out = fs;

    return 0;

err:
    close(fs->disk_fd);

    close(fs->journal_fd);

    free(fs->disk_path);

    free(fs->journal_path);

    free(fs);

    return -EIO;
}

void litefs_unmount(
    struct litefs_state *fs
) {
    if (!fs)
        return;

    journal_checkpoint(fs);

    off_t inode_region =
        (off_t)INODE_TABLE_OFFSET *
        LITEFS_BLOCK_SIZE;

    pwrite(
        fs->disk_fd,
        fs->inode_table,
        sizeof(fs->inode_table),
        inode_region
    );

    fs->sb.s_state = 0;

    sb_flush(fs);

    fsync(fs->disk_fd);

    close(fs->disk_fd);

    close(fs->journal_fd);

    pthread_mutex_destroy(
        &fs->fs_lock
    );

    pthread_mutex_destroy(
        &fs->cache.lock
    );

    free(fs->disk_path);

    free(fs->journal_path);

    free(fs);

    printf(
        "[unmount] LiteFS cleanly "
        "unmounted\n"
    );
}