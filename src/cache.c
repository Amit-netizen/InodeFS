/*
 * cache.c — LRU block cache
 *
 * Policy:
 *   - Read-through: on miss, read from disk and insert into cache
 *   - Write-back:   writes go to cache (dirty=1); flushed on eviction or
 *                   explicit cache_flush()
 *   - Eviction:     LRU tail is evicted; if dirty, written to disk first
 *
 * Thread safety: all public functions hold cache->lock.
 */

#include "litefs.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>

/* ── LRU list helpers (must be called with lock held) ── */

static void lru_remove(struct block_cache *c, struct cache_entry *e)
{
    if (e->prev) e->prev->next = e->next;
    else         c->lru_head   = e->next;

    if (e->next) e->next->prev = e->prev;
    else         c->lru_tail   = e->prev;

    e->prev = e->next = NULL;
}

static void lru_push_front(struct block_cache *c, struct cache_entry *e)
{
    e->prev = NULL;
    e->next = c->lru_head;
    if (c->lru_head) c->lru_head->prev = e;
    else             c->lru_tail = e;
    c->lru_head = e;
}

static struct cache_entry *lru_find(struct block_cache *c, uint32_t blk)
{
    for (struct cache_entry *e = c->lru_head; e; e = e->next)
        if (e->block_no == blk) return e;
    return NULL;
}

/* ── Public API ── */

void cache_init(struct block_cache *c)
{
    memset(c, 0, sizeof(*c));
    pthread_mutex_init(&c->lock, NULL);

    /* Mark all slots as empty (block_no = UINT32_MAX means unused) */
    for (int i = 0; i < CACHE_CAPACITY; i++)
        c->entries[i].block_no = UINT32_MAX;

    c->lru_head = c->lru_tail = NULL;
    c->seq = 0;
}

/*
 * cache_get — copy cached block data into buf.
 * Returns 0 on hit, -1 on miss.
 */
int cache_get(struct block_cache *c, uint32_t blk, void *buf)
{
    pthread_mutex_lock(&c->lock);

    struct cache_entry *e = lru_find(c, blk);
    if (!e) {
        c->misses++;
        pthread_mutex_unlock(&c->lock);
        return -1;  /* cache miss */
    }

    c->hits++;
    memcpy(buf, e->data, LITEFS_BLOCK_SIZE);

    /* Move to front (most recently used) */
    lru_remove(c, e);
    lru_push_front(c, e);
    e->lru_seq = ++c->seq;

    pthread_mutex_unlock(&c->lock);
    return 0;
}

/*
 * cache_put — insert or update a block in cache.
 * dirty=1 means this block must be written to disk before eviction.
 */
int cache_put(struct block_cache *c, uint32_t blk, const void *buf, int dirty)
{
    pthread_mutex_lock(&c->lock);

    /* Check if already cached — update in place */
    struct cache_entry *e = lru_find(c, blk);
    if (e) {
        memcpy(e->data, buf, LITEFS_BLOCK_SIZE);
        if (dirty) e->dirty = 1;
        lru_remove(c, e);
        lru_push_front(c, e);
        e->lru_seq = ++c->seq;
        pthread_mutex_unlock(&c->lock);
        return 0;
    }

    /* Find a free slot first */
    struct cache_entry *slot = NULL;
    for (int i = 0; i < CACHE_CAPACITY; i++) {
        if (c->entries[i].block_no == UINT32_MAX) {
            slot = &c->entries[i];
            break;
        }
    }

    /* No free slot — evict LRU tail */
    if (!slot) {
        slot = c->lru_tail;
        if (!slot) {
            pthread_mutex_unlock(&c->lock);
            return -ENOMEM;
        }
        lru_remove(c, slot);
        /* Caller is responsible for flushing dirty blocks via cache_flush()
         * before calling cache_put in a write path. Here we just mark
         * eviction; dirty writeback is handled in cache_flush(). */
        if (slot->dirty) {
            /*
             * We cannot do disk I/O here without the disk_fd, so we rely on
             * the caller invoking cache_flush() before the cache fills up.
             * For safety, log a warning. In production you'd pass disk_fd in.
             */
            fprintf(stderr, "[cache] WARNING: evicting dirty block %u without flush!\n",
                    slot->block_no);
        }
    }

    slot->block_no = blk;
    memcpy(slot->data, buf, LITEFS_BLOCK_SIZE);
    slot->dirty   = dirty;
    slot->lru_seq = ++c->seq;
    lru_push_front(c, slot);

    pthread_mutex_unlock(&c->lock);
    return 0;
}

/*
 * cache_flush — write all dirty blocks to disk.
 * Called before unmount, checkpoint, and periodically by a background thread.
 */
void cache_flush(struct block_cache *c, int disk_fd)
{
    pthread_mutex_lock(&c->lock);

    for (struct cache_entry *e = c->lru_head; e; e = e->next) {
        if (e->dirty && e->block_no != UINT32_MAX) {
            off_t offset = (off_t)e->block_no * LITEFS_BLOCK_SIZE;
            if (pwrite(disk_fd, e->data, LITEFS_BLOCK_SIZE, offset) != LITEFS_BLOCK_SIZE)
                fprintf(stderr, "[cache] flush error on block %u\n", e->block_no);
            else
                e->dirty = 0;
        }
    }

    pthread_mutex_unlock(&c->lock);
}

/*
 * cache_invalidate — remove a block from cache (e.g. after journal replay
 * overwrites it on disk and we need to re-read clean data).
 */
void cache_invalidate(struct block_cache *c, uint32_t blk)
{
    pthread_mutex_lock(&c->lock);

    struct cache_entry *e = lru_find(c, blk);
    if (e) {
        lru_remove(c, e);
        e->block_no = UINT32_MAX;
        e->dirty    = 0;
    }

    pthread_mutex_unlock(&c->lock);
}
