#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include "../common/timer.h"
#include "../common/cli_parser.h"

/**
 * Readers-Writers Problem: Readers-Preference Algorithm (Courtois et al., 1971).
 * Demonstrates Writer Starvation using POSIX Mutex and Semaphore (sem_t).
 *
 * SYSTEMS ARCHITECTURE NOTE:
 * 1. Why does Writer Starvation occur?
 *    - The first reader acquires the write_sem semaphore (sem_wait).
 *    - Subsequent readers only increment read_count under a lightweight count_mutex
 *      and enter the critical section immediately.
 *    - Only the last reader (read_count == 0) releases write_sem (sem_post).
 *    - If new readers keep arriving continuously, read_count never reaches 0,
 *      so the writer is starved and blocked indefinitely.
 * 2. Why POSIX Semaphore (sem_t) instead of pthread_mutex_t for write_sem?
 *    - In POSIX, a mutex has strict thread ownership: the thread that calls
 *      pthread_mutex_lock() MUST be the exact same thread that calls unlock.
 *    - Here, Reader Thread A might be the first reader (locking out writers),
 *      while Reader Thread B is the last reader (releasing writers).
 *    - A POSIX semaphore (sem_t) has no thread ownership, allowing cross-thread
 *      signaling (sem_wait in Thread A, sem_post in Thread B) without UB.
 */

typedef struct {
    int read_count;
    pthread_mutex_t count_mutex;
    sem_t write_sem;
    
    int64_t shared_data;
    
    uint64_t total_reads;
    uint64_t total_writes;
    uint64_t writer_max_wait_ns;
    uint64_t writer_total_wait_ns;
    bool stop_flag;
} rw_readers_pref_t;

static rw_readers_pref_t g_rw;
static cli_config_t g_config;

void rw_init(rw_readers_pref_t *rw) {
    rw->read_count = 0;
    pthread_mutex_init(&rw->count_mutex, NULL);
    sem_init(&rw->write_sem, 0, 1);
    rw->shared_data = 0;
    rw->total_reads = 0;
    rw->total_writes = 0;
    rw->writer_max_wait_ns = 0;
    rw->writer_total_wait_ns = 0;
    rw->stop_flag = false;
}

void rw_destroy(rw_readers_pref_t *rw) {
    pthread_mutex_destroy(&rw->count_mutex);
    sem_destroy(&rw->write_sem);
}

void *reader_thread(void *arg) {
    (void)arg;
    uint64_t local_reads = 0;

    while (!g_rw.stop_flag) {
        /* Reader entry */
        pthread_mutex_lock(&g_rw.count_mutex);
        g_rw.read_count++;
        if (g_rw.read_count == 1) {
            /* First reader locks out writers via binary semaphore */
            sem_wait(&g_rw.write_sem);
        }
        pthread_mutex_unlock(&g_rw.count_mutex);

        /* Read critical section (concurrent with other readers) */
        int64_t val = g_rw.shared_data;
        (void)val;
        local_reads++;

        /* Small work simulation */
        for (volatile int spin = 0; spin < 50; spin++);

        /* Reader exit */
        pthread_mutex_lock(&g_rw.count_mutex);
        g_rw.read_count--;
        if (g_rw.read_count == 0) {
            /* Last reader releases semaphore for waiting writers */
            sem_post(&g_rw.write_sem);
        }
        pthread_mutex_unlock(&g_rw.count_mutex);
    }

    pthread_mutex_lock(&g_rw.count_mutex);
    g_rw.total_reads += local_reads;
    pthread_mutex_unlock(&g_rw.count_mutex);

    return NULL;
}

void *writer_thread(void *arg) {
    (void)arg;

    while (!g_rw.stop_flag) {
        uint64_t wait_start = timer_now_ns();

        /* Writer requests exclusive access via binary semaphore */
        sem_wait(&g_rw.write_sem);

        uint64_t wait_end = timer_now_ns();
        uint64_t wait_time = timer_diff_ns(wait_start, wait_end);

        if (wait_time > g_rw.writer_max_wait_ns) {
            g_rw.writer_max_wait_ns = wait_time;
        }
        g_rw.writer_total_wait_ns += wait_time;

        /* Write critical section */
        g_rw.shared_data++;
        g_rw.total_writes++;

        for (volatile int spin = 0; spin < 100; spin++);

        sem_post(&g_rw.write_sem);

        usleep(1000); /* 1 ms pause between writes */
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
    printf(" READERS-WRITERS: READERS PREFERENCE\n");
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
    printf("  Maximum writer wait time:   %.2f ms (elevated due to reader stream)\n", max_writer_wait_ms);
    printf("  Final shared data value:    %lld\n", (long long)g_rw.shared_data);

    printf("\n[OBSERVATION] Heavy reader traffic dominates access:\n");
    printf("  Writer experiences significant wait times because read_count rarely drops to 0.\n");

    free(readers);
    rw_destroy(&g_rw);
    return 0;
}
