/*
 * flush_thread.c
 */

#define LITEFS_NO_FUSE

#include "litefs.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define FLUSH_INTERVAL_SEC 5

struct flush_thread_state {
    pthread_t tid;

    atomic_int stop;

    struct litefs_state *fs;
};

static struct flush_thread_state g_flush;

static void* flush_worker(void *arg) {
    struct flush_thread_state *state =
        (struct flush_thread_state *)arg;

    struct litefs_state *fs = state->fs;

    printf(
        "[flush_thread] started "
        "(interval=%ds)\n",
        FLUSH_INTERVAL_SEC
    );

    while (!atomic_load(&state->stop)) {
        for (int i = 0; i < FLUSH_INTERVAL_SEC; i++) {
            if (atomic_load(&state->stop))
                break;

            sleep(1);
        }

        if (atomic_load(&state->stop))
            break;

        pthread_mutex_lock(&fs->fs_lock);

        uint32_t dirty = 0;

        for (int i = 0; i < CACHE_CAPACITY; i++) {
            if (fs->cache.entries[i].dirty)
                dirty++;
        }

        if (dirty > 0) {
            printf(
                "[flush_thread] flushing "
                "%u dirty blocks\n",
                dirty
            );

            journal_checkpoint(fs);
        }

        pthread_mutex_unlock(&fs->fs_lock);
    }

    printf("[flush_thread] exiting\n");

    return NULL;
}

int flush_thread_start(struct litefs_state *fs) {
    atomic_store(&g_flush.stop, 0);

    g_flush.fs = fs;

    int ret = pthread_create(
        &g_flush.tid,
        NULL,
        flush_worker,
        &g_flush
    );

    if (ret != 0) {
        fprintf(
            stderr,
            "[flush_thread] pthread_create failed: %d\n",
            ret
        );

        return -1;
    }

    return 0;
}

void flush_thread_stop(struct litefs_state *fs) {
    (void)fs;

    atomic_store(&g_flush.stop, 1);

    pthread_join(g_flush.tid, NULL);
}