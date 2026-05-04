/*
 * fuse_ops.c
 */

#define _GNU_SOURCE

#ifndef FUSE_USE_VERSION
    #define FUSE_USE_VERSION 31
#endif

#include "litefs.h"

#include <errno.h>
#include <fuse.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>

static struct litefs_state* get_fs(void) {
    return (struct litefs_state *)
        fuse_get_context()->private_data;
}

static void inode_to_stat(
    const struct litefs_inode *inode,
    struct stat *st
) {
    memset(st, 0, sizeof(*st));

    st->st_ino = inode->i_ino;

    st->st_mode = inode->i_mode;

    switch (inode->i_type) {
        case LITEFS_INODE_DIR:
            st->st_mode |= S_IFDIR;
            break;

        case LITEFS_INODE_FILE:
            st->st_mode |= S_IFREG;
            break;

        case LITEFS_INODE_SYMLINK:
            st->st_mode |= S_IFLNK;
            break;
    }

    st->st_nlink = inode->i_links;

    st->st_uid = inode->i_uid;

    st->st_gid = inode->i_gid;

    st->st_size = (off_t)inode->i_size;

    st->st_blocks =
        inode->i_blocks *
        (LITEFS_BLOCK_SIZE / 512);

    st->st_blksize = LITEFS_BLOCK_SIZE;

    st->st_atime = (time_t)inode->i_atime;
    st->st_mtime = (time_t)inode->i_mtime;
    st->st_ctime = (time_t)inode->i_ctime;
}

static int lfs_getattr(
    const char *path,
    struct stat *st,
    struct fuse_file_info *fi
) {
    (void)fi;

    struct litefs_state *fs = get_fs();

    pthread_mutex_lock(&fs->fs_lock);

    uint32_t ino;

    int ret = path_resolve(fs, path, &ino);

    if (ret < 0) {
        pthread_mutex_unlock(&fs->fs_lock);
        return ret;
    }

    struct litefs_inode *inode =
        inode_get(fs, ino);

    if (!inode) {
        pthread_mutex_unlock(&fs->fs_lock);
        return -ENOENT;
    }

    inode_to_stat(inode, st);

    pthread_mutex_unlock(&fs->fs_lock);

    return 0;
}

static int lfs_readdir(
    const char *path,
    void *buf,
    fuse_fill_dir_t filler,
    off_t offset,
    struct fuse_file_info *fi,
    enum fuse_readdir_flags flags
) {
    (void)offset;
    (void)fi;
    (void)flags;

    struct litefs_state *fs = get_fs();

    pthread_mutex_lock(&fs->fs_lock);

    uint32_t ino;

    int ret = path_resolve(fs, path, &ino);

    if (ret < 0) {
        pthread_mutex_unlock(&fs->fs_lock);
        return ret;
    }

    filler(buf, ".", NULL, 0, 0);

    filler(buf, "..", NULL, 0, 0);

    ret = dir_readdir(fs, ino, buf, filler);

    pthread_mutex_unlock(&fs->fs_lock);

    return ret;
}

static int lfs_mkdir(
    const char *path,
    mode_t mode
) {
    struct litefs_state *fs = get_fs();

    pthread_mutex_lock(&fs->fs_lock);

    uint32_t parent_ino;

    char name[LITEFS_NAME_MAX + 1];

    int ret = path_resolve_parent(
        fs,
        path,
        &parent_ino,
        name
    );

    if (ret < 0) {
        pthread_mutex_unlock(&fs->fs_lock);
        return ret;
    }

    uint32_t new_ino;

    ret = inode_alloc(
        fs,
        LITEFS_INODE_DIR,
        mode & 0777,
        &new_ino
    );

    if (ret < 0) {
        pthread_mutex_unlock(&fs->fs_lock);
        return ret;
    }

    ret = dir_add_entry(
        fs,
        parent_ino,
        name,
        new_ino,
        LITEFS_INODE_DIR
    );

    if (ret < 0) {
        inode_free(fs, new_ino);

        pthread_mutex_unlock(&fs->fs_lock);

        return ret;
    }

    dir_add_entry(
        fs,
        new_ino,
        ".",
        new_ino,
        LITEFS_INODE_DIR
    );

    dir_add_entry(
        fs,
        new_ino,
        "..",
        parent_ino,
        LITEFS_INODE_DIR
    );

    pthread_mutex_unlock(&fs->fs_lock);

    return 0;
}

static int lfs_create(
    const char *path,
    mode_t mode,
    struct fuse_file_info *fi
) {
    (void)fi;

    struct litefs_state *fs = get_fs();

    pthread_mutex_lock(&fs->fs_lock);

    uint32_t parent_ino;

    char name[LITEFS_NAME_MAX + 1];

    int ret = path_resolve_parent(
        fs,
        path,
        &parent_ino,
        name
    );

    if (ret < 0) {
        pthread_mutex_unlock(&fs->fs_lock);
        return ret;
    }

    uint32_t new_ino;

    ret = inode_alloc(
        fs,
        LITEFS_INODE_FILE,
        mode & 0777,
        &new_ino
    );

    if (ret < 0) {
        pthread_mutex_unlock(&fs->fs_lock);
        return ret;
    }

    ret = dir_add_entry(
        fs,
        parent_ino,
        name,
        new_ino,
        LITEFS_INODE_FILE
    );

    if (ret < 0) {
        inode_free(fs, new_ino);

        pthread_mutex_unlock(&fs->fs_lock);

        return ret;
    }

    fi->fh = new_ino;

    pthread_mutex_unlock(&fs->fs_lock);

    return 0;
}

static int lfs_open(
    const char *path,
    struct fuse_file_info *fi
) {
    struct litefs_state *fs = get_fs();

    pthread_mutex_lock(&fs->fs_lock);

    uint32_t ino;

    int ret = path_resolve(fs, path, &ino);

    if (ret < 0) {
        pthread_mutex_unlock(&fs->fs_lock);
        return ret;
    }

    fi->fh = ino;

    pthread_mutex_unlock(&fs->fs_lock);

    return 0;
}

static int lfs_read(
    const char *path,
    char *buf,
    size_t size,
    off_t offset,
    struct fuse_file_info *fi
) {
    (void)path;

    struct litefs_state *fs = get_fs();

    pthread_mutex_lock(&fs->fs_lock);

    struct litefs_inode *inode =
        inode_get(fs, (uint32_t)fi->fh);

    if (!inode) {
        pthread_mutex_unlock(&fs->fs_lock);
        return -ENOENT;
    }

    ssize_t n = file_read(
        fs,
        inode,
        buf,
        size,
        offset
    );

    pthread_mutex_unlock(&fs->fs_lock);

    return (int)n;
}

static int lfs_write(
    const char *path,
    const char *buf,
    size_t size,
    off_t offset,
    struct fuse_file_info *fi
) {
    (void)path;

    struct litefs_state *fs = get_fs();

    pthread_mutex_lock(&fs->fs_lock);

    struct litefs_inode *inode =
        inode_get(fs, (uint32_t)fi->fh);

    if (!inode) {
        pthread_mutex_unlock(&fs->fs_lock);
        return -ENOENT;
    }

    ssize_t n = file_write(
        fs,
        inode,
        buf,
        size,
        offset
    );

    pthread_mutex_unlock(&fs->fs_lock);

    return (int)n;
}

static int lfs_truncate(
    const char *path,
    off_t size,
    struct fuse_file_info *fi
) {
    (void)fi;

    struct litefs_state *fs = get_fs();

    pthread_mutex_lock(&fs->fs_lock);

    uint32_t ino;

    int ret = path_resolve(fs, path, &ino);

    if (ret < 0) {
        pthread_mutex_unlock(&fs->fs_lock);
        return ret;
    }

    struct litefs_inode *inode =
        inode_get(fs, ino);

    if (!inode) {
        pthread_mutex_unlock(&fs->fs_lock);
        return -ENOENT;
    }

    inode->i_size = (uint64_t)size;

    inode->i_mtime = time(NULL);

    pthread_mutex_unlock(&fs->fs_lock);

    return 0;
}

static int lfs_release(
    const char *path,
    struct fuse_file_info *fi
) {
    (void)path;
    (void)fi;

    struct litefs_state *fs = get_fs();

    cache_flush(&fs->cache, fs->disk_fd);

    return 0;
}

static int lfs_fsync(
    const char *path,
    int datasync,
    struct fuse_file_info *fi
) {
    (void)path;
    (void)datasync;
    (void)fi;

    struct litefs_state *fs = get_fs();

    pthread_mutex_lock(&fs->fs_lock);

    journal_checkpoint(fs);

    pthread_mutex_unlock(&fs->fs_lock);

    return 0;
}

static void* lfs_init(
    struct fuse_conn_info *conn,
    struct fuse_config *cfg
) {
    (void)conn;

#if FUSE_USE_VERSION >= 30
    cfg->kernel_cache = 0;
    cfg->use_ino = 1;
#endif

    struct litefs_state *fs =
        fuse_get_context()->private_data;

    flush_thread_start(fs);

    return fs;
}

static void lfs_destroy(void *private_data) {
    struct litefs_state *fs =
        (struct litefs_state *)private_data;

    flush_thread_stop(fs);

    litefs_unmount(fs);
}

static int lfs_getxattr(
    const char *path,
    const char *name,
    char *value,
    size_t size
) {
    (void)path;
    (void)name;
    (void)value;
    (void)size;

    return -ENOTSUP;
}

static int lfs_listxattr(
    const char *path,
    char *list,
    size_t size
) {
    (void)path;
    (void)list;
    (void)size;

    return 0;
}

static int lfs_setxattr(
    const char *path,
    const char *name,
    const char *value,
    size_t size,
    int flags
) {
    (void)path;
    (void)name;
    (void)value;
    (void)size;
    (void)flags;

    return -ENOTSUP;
}

static int lfs_removexattr(
    const char *path,
    const char *name
) {
    (void)path;
    (void)name;

    return -ENOTSUP;
}

struct fuse_operations litefs_ops = {
    .getattr = lfs_getattr,
    .readdir = lfs_readdir,

    .mkdir = lfs_mkdir,

    .create = lfs_create,
    .truncate = lfs_truncate,

    .open = lfs_open,
    .read = lfs_read,
    .write = lfs_write,

    .release = lfs_release,
    .fsync = lfs_fsync,

    .getxattr = lfs_getxattr,
    .listxattr = lfs_listxattr,
    .setxattr = lfs_setxattr,
    .removexattr = lfs_removexattr,

    .init = lfs_init,
    .destroy = lfs_destroy,
};