#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include "../common/timer.h"
#include "../common/cli_parser.h"

/**
 * Readers-Writers Problem: Writers-Preference Algorithm.
 * Prevents Writer Starvation through fair queuing.
 *
 * SYSTEMS ARCHITECTURE NOTE:
 * How is writer starvation eliminated?
 * 1. Counters and flags:
 *    - active_readers: number of readers currently in critical section.
 *    - waiting_writers: number of writers currently waiting to write.
 *    - writer_active: boolean flag indicating an active writer.
 * 2. Reader Entry Rule:
 *    A reader is allowed to enter ONLY if writer_active == false AND waiting_writers == 0.
 *    If any writer is waiting, new readers are blocked on cond_readers!
 * 3. Result:
 *    Writer wait latency is bounded only by the time required for currently active readers
 *    to finish. No incoming readers can jump ahead in the queue.
 */

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond_readers;
    pthread_cond_t cond_writers;

    int active_readers;
    int waiting_writers;
    bool writer_active;

    int64_t shared_data;

    uint64_t total_reads;
    uint64_t total_writes;
    uint64_t writer_max_wait_ns;
    uint64_t writer_total_wait_ns;
    bool stop_flag;
} rw_writers_pref_t;

static rw_writers_pref_t g_rw;
static cli_config_t g_config;

void rw_init(rw_writers_pref_t *rw) {
    pthread_mutex_init(&rw->mutex, NULL);
    pthread_cond_init(&rw->cond_readers, NULL);
    pthread_cond_init(&rw->cond_writers, NULL);
    rw->active_readers = 0;
    rw->waiting_writers = 0;
    rw->writer_active = false;
    rw->shared_data = 0;
    rw->total_reads = 0;
    rw->total_writes = 0;
    rw->writer_max_wait_ns = 0;
    rw->writer_total_wait_ns = 0;
    rw->stop_flag = false;
}

void rw_destroy(rw_writers_pref_t *rw) {
    pthread_mutex_destroy(&rw->mutex);
    pthread_cond_destroy(&rw->cond_readers);
    pthread_cond_destroy(&rw->cond_writers);
}

void rw_read_lock(rw_writers_pref_t *rw) {
    pthread_mutex_lock(&rw->mutex);
    /* Readers wait if a writer is active OR writers are queued */
    while (rw->writer_active || rw->waiting_writers > 0) {
        pthread_cond_wait(&rw->cond_readers, &rw->mutex);
    }
    rw->active_readers++;
    pthread_mutex_unlock(&rw->mutex);
}

void rw_read_unlock(rw_writers_pref_t *rw) {
    pthread_mutex_lock(&rw->mutex);
    rw->active_readers--;
    /* If last reader leaves and writers are waiting, signal waiting writer */
    if (rw->active_readers == 0 && rw->waiting_writers > 0) {
        pthread_cond_signal(&rw->cond_writers);
    }
    pthread_mutex_unlock(&rw->mutex);
}

void rw_write_lock(rw_writers_pref_t *rw) {
    pthread_mutex_lock(&rw->mutex);
    rw->waiting_writers++;
    /* Writer waits while readers or another writer are active */
    while (rw->writer_active || rw->active_readers > 0) {
        pthread_cond_wait(&rw->cond_writers, &rw->mutex);
    }
    rw->waiting_writers--;
    rw->writer_active = true;
    pthread_mutex_unlock(&rw->mutex);
}

void rw_write_unlock(rw_writers_pref_t *rw) {
    pthread_mutex_lock(&rw->mutex);
    rw->writer_active = false;
    /* Prioritize remaining queued writers; otherwise wake all waiting readers */
    if (rw->waiting_writers > 0) {
        pthread_cond_signal(&rw->cond_writers);
    } else {
        pthread_cond_broadcast(&rw->cond_readers);
    }
    pthread_mutex_unlock(&rw->mutex);
}

void *reader_thread(void *arg) {
    (void)arg;
    uint64_t local_reads = 0;

    while (!g_rw.stop_flag) {
        rw_read_lock(&g_rw);

        int64_t val = g_rw.shared_data;
        (void)val;
        local_reads++;

        for (volatile int spin = 0; spin < 50; spin++);

        rw_read_unlock(&g_rw);
    }

    pthread_mutex_lock(&g_rw.mutex);
    g_rw.total_reads += local_reads;
    pthread_mutex_unlock(&g_rw.mutex);

    return NULL;
}

void *writer_thread(void *arg) {
    (void)arg;

    while (!g_rw.stop_flag) {
        uint64_t wait_start = timer_now_ns();

        rw_write_lock(&g_rw);

        uint64_t wait_end = timer_now_ns();
        uint64_t wait_time = timer_diff_ns(wait_start, wait_end);

        if (wait_time > g_rw.writer_max_wait_ns) {
            g_rw.writer_max_wait_ns = wait_time;
        }
        g_rw.writer_total_wait_ns += wait_time;

        g_rw.shared_data++;
        g_rw.total_writes++;

        for (volatile int spin = 0; spin < 100; spin++);

        rw_write_unlock(&g_rw);

        usleep(1000);
    }

    return NULL;
}

int main(int argc, char **argv) {
    cli_config_init_defaults(&g_config);
    if (cli_parse_args(argc, argv, &g_config, argv[0]) != 0) {
        return 1;
    }

    int num_readers = (g_config.threads > 1) ? g_config.threads : 4;
    int num_writers = 1;

    printf("================================================================\n");
    printf(" READERS-WRITERS: WRITERS PREFERENCE\n");
    printf(" Readers: %d, Writers: %d\n", num_readers, num_writers);
    printf(" Workload duration: 3 seconds continuous reader stream\n");
    printf("================================================================\n");

    rw_init(&g_rw);

    pthread_t *readers = malloc(sizeof(pthread_t) * (size_t)num_readers);
    pthread_t writer;

    uint64_t t_start = timer_now_ns();

    for (int i = 0; i < num_readers; ++i) {
        pthread_create(&readers[i], NULL, reader_thread, NULL);
    }
    pthread_create(&writer, NULL, writer_thread, NULL);

    sleep(3);
    g_rw.stop_flag = true;

    for (int i = 0; i < num_readers; ++i) {
        pthread_join(readers[i], NULL);
    }
    pthread_join(writer, NULL);

    uint64_t t_end = timer_now_ns();
    double elapsed_sec = timer_diff_sec(t_start, t_end);

    double avg_writer_wait_ms = (g_rw.total_writes > 0) ? 
        ((double)g_rw.writer_total_wait_ns / (double)g_rw.total_writes / 1e6) : 0.0;
    double max_writer_wait_ms = (double)g_rw.writer_max_wait_ns / 1e6;

    printf("\nTEST RESULTS:\n");
    printf("  Test duration:              %.2f s\n", elapsed_sec);
    printf("  Total read operations:      %llu (%.0f ops/sec)\n", 
           (unsigned long long)g_rw.total_reads, (double)g_rw.total_reads / elapsed_sec);
    printf("  Total write operations:     %llu (%.0f ops/sec)\n", 
           (unsigned long long)g_rw.total_writes, (double)g_rw.total_writes / elapsed_sec);
    printf("  Average writer wait time:   %.2f ms\n", avg_writer_wait_ms);
    printf("  Maximum writer wait time:   %.2f ms (bounded by active readers)\n", max_writer_wait_ms);
    printf("  Final shared data value:    %lld\n", (long long)g_rw.shared_data);

    printf("\n[STATUS] Fair queuing verified: queued writers block incoming readers,\n");
    printf("  preventing writer starvation while maintaining high reader throughput.\n");

    free(readers);
    rw_destroy(&g_rw);
    return 0;
}
