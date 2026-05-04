#ifndef LITEFS_H
#define LITEFS_H

/*
 * LiteFS - A FUSE-based filesystem with journaling and caching
 *
 * Architecture:
 *   Disk layout (single backing file):
 *   [ Superblock (1 block) | Inode Table (N blocks) | Block Bitmap (M blocks) | Data Blocks ]
 *
 *   Journal (separate backing file):
 *   [ Journal Header | Circular log of transactions ]
 *
 *   Cache: LRU in-memory block cache sitting between FUSE ops and disk I/O
 */

#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>   /* off_t, ssize_t, uid_t, gid_t, mode_t */
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/*
 * FUSE headers only needed by the FUSE daemon binary (litefs).
 * Compile with -DLITEFS_NO_FUSE for fsck and unit tests so they
 * do not require libfuse to be installed.
 */
#ifndef LITEFS_NO_FUSE
#  define FUSE_USE_VERSION 31
#  include <fuse.h>
#else
   /* Minimal stub for the fuse_fill_dir_t used in dir_readdir API */
   typedef int (*fuse_fill_dir_t)(void *buf, const char *name,
                                  const struct stat *stbuf, off_t off, int flags);
#endif

/* ───────────────────────────── Geometry ───────────────────────────── */

#define LITEFS_BLOCK_SIZE       4096          /* bytes per block            */
#define LITEFS_MAGIC            0x4C495445    /* "LITE"                     */
#define LITEFS_VERSION          1

#define LITEFS_MAX_INODES       1024          /* max files/dirs             */
#define LITEFS_MAX_BLOCKS       8192          /* total data blocks          */
#define LITEFS_DIRECT_BLOCKS    12            /* direct block pointers      */
#define LITEFS_NAME_MAX         255
#define LITEFS_DIR_ENTRIES      128           /* entries per dir block      */

/* Derived layout constants (in block numbers) */
#define SUPERBLOCK_OFFSET       0             /* block 0                    */
#define INODE_TABLE_OFFSET      1             /* blocks 1..N                */
#define INODE_TABLE_BLOCKS      ((LITEFS_MAX_INODES * sizeof(struct litefs_inode) + LITEFS_BLOCK_SIZE - 1) / LITEFS_BLOCK_SIZE)
#define BITMAP_OFFSET           (INODE_TABLE_OFFSET + INODE_TABLE_BLOCKS)
#define BITMAP_BLOCKS           ((LITEFS_MAX_BLOCKS + 8 * LITEFS_BLOCK_SIZE - 1) / (8 * LITEFS_BLOCK_SIZE))
#define DATA_OFFSET             (BITMAP_OFFSET + BITMAP_BLOCKS)

/* ───────────────────────────── Inode ──────────────────────────────── */

#define LITEFS_INODE_FREE       0
#define LITEFS_INODE_FILE       1
#define LITEFS_INODE_DIR        2
#define LITEFS_INODE_SYMLINK    3

struct litefs_inode {
    uint32_t  i_ino;                             /* inode number            */
    uint8_t   i_type;                            /* file/dir/symlink        */
    uint16_t  i_mode;                            /* permission bits         */
    uint32_t  i_uid;
    uint32_t  i_gid;
    uint64_t  i_size;                            /* bytes                   */
    uint64_t  i_atime;
    uint64_t  i_mtime;
    uint64_t  i_ctime;
    uint32_t  i_links;                           /* hard link count         */
    uint32_t  i_blocks;                          /* allocated data blocks   */
    uint32_t  i_direct[LITEFS_DIRECT_BLOCKS];    /* direct block ptrs       */
    uint32_t  i_indirect;                        /* single indirect block   */
    uint32_t  i_dindirect;                       /* double indirect block   */
    uint8_t   _pad[8];
} __attribute__((packed));

/* ───────────────────────────── Directory entry ─────────────────────── */

struct litefs_dirent {
    uint32_t  d_ino;
    uint8_t   d_type;
    uint8_t   d_namelen;
    char      d_name[LITEFS_NAME_MAX + 1];
} __attribute__((packed));

/* ───────────────────────────── Superblock ──────────────────────────── */

struct litefs_superblock {
    uint32_t  s_magic;
    uint32_t  s_version;
    uint32_t  s_block_size;
    uint32_t  s_total_blocks;
    uint32_t  s_free_blocks;
    uint32_t  s_total_inodes;
    uint32_t  s_free_inodes;
    uint32_t  s_root_ino;
    uint64_t  s_mount_time;
    uint64_t  s_write_time;
    uint32_t  s_state;           /* 0=clean, 1=dirty (needs journal replay) */
    uint8_t   _pad[4060 - 48];
} __attribute__((packed));

/* ───────────────────────────── Journal ─────────────────────────────── */

#define JOURNAL_MAGIC           0x4A4C4954    /* "JLIT"                     */
#define JOURNAL_SIZE_BLOCKS     256           /* circular log capacity      */
#define JOURNAL_BLOCK_COMMIT    0x01          /* record types               */
#define JOURNAL_BLOCK_DATA      0x02
#define JOURNAL_BLOCK_REVOKE    0x03

struct journal_header {
    uint32_t  j_magic;
    uint32_t  j_version;
    uint32_t  j_head;            /* next write position (block index)       */
    uint32_t  j_tail;            /* oldest un-checkpointed transaction      */
    uint32_t  j_size;            /* total journal blocks                    */
    uint32_t  j_sequence;        /* monotonic transaction counter           */
    uint8_t   _pad[4072];
} __attribute__((packed));

struct journal_block_header {
    uint32_t  jb_magic;
    uint32_t  jb_type;           /* COMMIT / DATA / REVOKE                  */
    uint32_t  jb_sequence;       /* transaction this belongs to             */
    uint32_t  jb_target_block;   /* which FS block this data is for         */
    uint32_t  jb_checksum;       /* simple CRC32 of the data block          */
    uint8_t   _pad[12];
} __attribute__((packed));

/* ───────────────────────────── LRU Cache ───────────────────────────── */

#define CACHE_CAPACITY          64            /* cached blocks              */

struct cache_entry {
    uint32_t  block_no;
    uint8_t   data[LITEFS_BLOCK_SIZE];
    int       dirty;
    uint64_t  lru_seq;
    struct cache_entry *prev, *next;          /* LRU doubly-linked list     */
};

struct block_cache {
    struct cache_entry  entries[CACHE_CAPACITY];
    struct cache_entry *lru_head;             /* most recently used         */
    struct cache_entry *lru_tail;             /* least recently used        */
    uint64_t            seq;                  /* monotonic counter          */
    uint32_t            hits;
    uint32_t            misses;
    pthread_mutex_t     lock;
};

/* ───────────────────────────── FS state ────────────────────────────── */

struct litefs_state {
    int                   disk_fd;
    int                   journal_fd;
    char                 *disk_path;
    char                 *journal_path;

    struct litefs_superblock sb;
    uint8_t               block_bitmap[LITEFS_MAX_BLOCKS / 8];
    struct litefs_inode   inode_table[LITEFS_MAX_INODES];

    struct block_cache    cache;
    pthread_mutex_t       fs_lock;            /* coarse-grained FS lock     */

    /* journal state */
    struct journal_header jh;
    uint32_t              cur_txn_seq;
};

/* ───────────────────────────── API ─────────────────────────────────── */

/* init / teardown */
int  litefs_init_disk(const char *disk_path, const char *journal_path, uint32_t total_blocks);
int  litefs_mount(const char *disk_path, const char *journal_path, struct litefs_state **out);
void litefs_unmount(struct litefs_state *fs);

/* inode ops */
struct litefs_inode *inode_get(struct litefs_state *fs, uint32_t ino);
int   inode_alloc(struct litefs_state *fs, uint8_t type, uint16_t mode, uint32_t *out_ino);
int   inode_free(struct litefs_state *fs, uint32_t ino);
int   inode_flush(struct litefs_state *fs, uint32_t ino);

/* block ops */
int   block_alloc(struct litefs_state *fs, uint32_t *out_blk);
int   block_free(struct litefs_state *fs, uint32_t blk);
int   block_read(struct litefs_state *fs, uint32_t blk, void *buf);
int   block_write(struct litefs_state *fs, uint32_t blk, const void *buf);

/* file data ops */
ssize_t file_read(struct litefs_state *fs, struct litefs_inode *ino,
                  void *buf, size_t size, off_t offset);
ssize_t file_write(struct litefs_state *fs, struct litefs_inode *ino,
                   const void *buf, size_t size, off_t offset);
int     file_truncate(struct litefs_state *fs, struct litefs_inode *ino, off_t size);

/* directory ops */
int dir_lookup(struct litefs_state *fs, uint32_t dir_ino, const char *name, uint32_t *out_ino);
int dir_add_entry(struct litefs_state *fs, uint32_t dir_ino, const char *name, uint32_t ino, uint8_t type);
int dir_remove_entry(struct litefs_state *fs, uint32_t dir_ino, const char *name);
int dir_readdir(struct litefs_state *fs, uint32_t dir_ino,
                void *buf, fuse_fill_dir_t filler);

/* journal ops */
int  journal_begin_txn(struct litefs_state *fs);
int  journal_log_block(struct litefs_state *fs, uint32_t block_no, const void *data);
int  journal_commit_txn(struct litefs_state *fs);
int  journal_replay(struct litefs_state *fs);
void journal_checkpoint(struct litefs_state *fs);

/* cache ops */
void cache_init(struct block_cache *c);
int  cache_get(struct block_cache *c, uint32_t blk, void *buf);
int  cache_put(struct block_cache *c, uint32_t blk, const void *buf, int dirty);
void cache_flush(struct block_cache *c, int disk_fd);
void cache_invalidate(struct block_cache *c, uint32_t blk);

/* path helpers */
int  path_resolve(struct litefs_state *fs, const char *path, uint32_t *out_ino);
int  path_resolve_parent(struct litefs_state *fs, const char *path,
                         uint32_t *out_parent_ino, char *out_name);

/* superblock */
int  sb_flush(struct litefs_state *fs);
int  bitmap_flush(struct litefs_state *fs);

/* FUSE ops (registered in main) */
extern struct fuse_operations litefs_ops;

/* Utility */
uint32_t crc32_simple(const void *data, size_t len);
uint64_t litefs_now(void);

/* flush thread */
int  flush_thread_start(struct litefs_state *fs);
void flush_thread_stop(struct litefs_state *fs);

#define LITEFS_ROOT_INO  1
#define ERR(x)           (-(x))

#endif /* LITEFS_H */
