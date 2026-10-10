#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include "buffer.h"
#include "../common/timer.h"
#include "../common/cli_parser.h"

/**
 * Synchronized Producer-Consumer using POSIX Mutex and 2 Condition Variables.
 *
 * SYSTEMS ARCHITECTURE NOTE:
 * Why check condition using while() instead of if()?
 * 1. Spurious Wakeups:
 *    POSIX standard explicitly allows pthread_cond_wait() to wake up even if no
 *    signal or broadcast was sent (e.g., interrupted by OS kernel signal).
 * 2. Mesa Semantics (Signal-and-Continue):
 *    POSIX uses Mesa semantics. When pthread_cond_signal wakes a thread, it does NOT
 *    immediately get the CPU. It just moves to the ready queue for the mutex.
 *    By the time it acquires the mutex, another thread might have already emptied
 *    or filled the buffer. Thus, condition MUST be re-checked in a while() loop!
 * 3. Atomicity of pthread_cond_wait(&cond, &mutex):
 *    It atomically releases the mutex AND puts calling thread to sleep.
 *    This prevents lost wakeups. Upon wakeup, it atomically re-acquires the mutex.
 * 4. Why TWO condition variables?
 *    - cond_not_empty: signals consumers that an item is available.
 *    - cond_not_full: signals producers that a buffer slot is free.
 *    Using a single condvar would cause a "thundering herd problem" with broadcast.
 */

typedef struct {
    ring_buffer_t buffer;
    pthread_mutex_t mutex;
    pthread_cond_t cond_not_empty;
    pthread_cond_t cond_not_full;
    bool finished;           /* True when all producers have finished */
    int active_producers;    /* Count of active producer threads */
} sync_queue_t;

static sync_queue_t g_queue;
static cli_config_t g_config;

static uint64_t g_total_produced = 0;
static uint64_t g_total_consumed = 0;
static uint64_t g_checksum_errors = 0;
static uint64_t g_total_latency_ns = 0;

typedef struct {
    int thread_id;
    size_t items_to_produce;
} producer_arg_t;

void sync_queue_init(sync_queue_t *sq, size_t capacity, int num_producers) {
    ring_buffer_init(&sq->buffer, capacity);
    pthread_mutex_init(&sq->mutex, NULL);
    pthread_cond_init(&sq->cond_not_empty, NULL);
    pthread_cond_init(&sq->cond_not_full, NULL);
    sq->finished = false;
    sq->active_producers = num_producers;
}

void sync_queue_destroy(sync_queue_t *sq) {
    pthread_mutex_destroy(&sq->mutex);
    pthread_cond_destroy(&sq->cond_not_empty);
    pthread_cond_destroy(&sq->cond_not_full);
    ring_buffer_destroy(&sq->buffer);
}

void *sync_producer_thread(void *arg) {
    producer_arg_t *parg = (producer_arg_t *)arg;
    int id = parg->thread_id;
    size_t total = parg->items_to_produce;

    for (size_t seq = 0; seq < total; ++seq) {
        buffer_item_t item;
        item.producer_id = id;
        item.seq_num = (int)seq;
        item.timestamp_ns = timer_now_ns();
        item.checksum = buffer_item_calc_checksum(id, (int)seq);

        /* Enter critical section */
        pthread_mutex_lock(&g_queue.mutex);

        /* Wait while buffer is full (while loop guards against spurious wakeups) */
        while (ring_buffer_is_full(&g_queue.buffer)) {
            pthread_cond_wait(&g_queue.cond_not_full, &g_queue.mutex);
        }

        /* Safe push into bounded buffer */
        ring_buffer_push_unsafe(&g_queue.buffer, item);

        /* Notify waiting consumers that buffer is not empty */
        pthread_cond_signal(&g_queue.cond_not_empty);

        /* Exit critical section */
        pthread_mutex_unlock(&g_queue.mutex);
    }

    /* Producer finished: update active producer counter */
    pthread_mutex_lock(&g_queue.mutex);
    g_total_produced += total;
    g_queue.active_producers--;
    if (g_queue.active_producers == 0) {
        g_queue.finished = true;
        /* Broadcast to wake up any remaining waiting consumers */
        pthread_cond_broadcast(&g_queue.cond_not_empty);
    }
    pthread_mutex_unlock(&g_queue.mutex);

    return NULL;
}

void *sync_consumer_thread(void *arg) {
    (void)arg;
    uint64_t local_consumed = 0;
    uint64_t local_errors = 0;
    uint64_t local_latency = 0;

    while (1) {
        buffer_item_t item;

        pthread_mutex_lock(&g_queue.mutex);

        /* Wait while buffer is empty and producers are still working */
        while (ring_buffer_is_empty(&g_queue.buffer) && !g_queue.finished) {
            pthread_cond_wait(&g_queue.cond_not_empty, &g_queue.mutex);
        }

        /* If empty and all producers are done, break out of loop */
        if (ring_buffer_is_empty(&g_queue.buffer) && g_queue.finished) {
            pthread_mutex_unlock(&g_queue.mutex);
            break;
        }

        /* Safe pop from buffer */
        bool ok = ring_buffer_pop_unsafe(&g_queue.buffer, &item);

        /* Signal producers that free space is now available */
        pthread_cond_signal(&g_queue.cond_not_full);

        pthread_mutex_unlock(&g_queue.mutex);

        if (ok) {
            uint64_t now = timer_now_ns();
            local_consumed++;
            if (now >= item.timestamp_ns) {
                local_latency += (now - item.timestamp_ns);
            }

            uint32_t expected = buffer_item_calc_checksum(item.producer_id, item.seq_num);
            if (item.checksum != expected) {
                local_errors++;
            }
        }
    }

    /* Aggregate metrics under mutex */
    pthread_mutex_lock(&g_queue.mutex);
    g_total_consumed += local_consumed;
    g_checksum_errors += local_errors;
    g_total_latency_ns += local_latency;
    pthread_mutex_unlock(&g_queue.mutex);

    return NULL;
}

int main(int argc, char **argv) {
    cli_config_init_defaults(&g_config);
    if (cli_parse_args(argc, argv, &g_config, argv[0]) != 0) {
        return 1;
    }

    printf("================================================================\n");
    printf(" SYNCHRONIZED PRODUCER-CONSUMER (MUTEX + 2 CONDVARS)\n");
    printf(" Producers: %d, Consumers: %d\n", g_config.threads, g_config.threads);
    printf(" Buffer capacity: %zu, Items per thread: %zu\n", g_config.buffer_size, g_config.items);
    printf("================================================================\n");

    sync_queue_init(&g_queue, g_config.buffer_size, g_config.threads);

    pthread_t *prod_threads = malloc(sizeof(pthread_t) * (size_t)g_config.threads);
    pthread_t *cons_threads = malloc(sizeof(pthread_t) * (size_t)g_config.threads);
    producer_arg_t *pargs = malloc(sizeof(producer_arg_t) * (size_t)g_config.threads);

    uint64_t t_start = timer_now_ns();

    for (int i = 0; i < g_config.threads; ++i) {
        pthread_create(&cons_threads[i], NULL, sync_consumer_thread, NULL);
    }

    for (int i = 0; i < g_config.threads; ++i) {
        pargs[i].thread_id = i;
        pargs[i].items_to_produce = g_config.items;
        pthread_create(&prod_threads[i], NULL, sync_producer_thread, &pargs[i]);
    }

    for (int i = 0; i < g_config.threads; ++i) {
        pthread_join(prod_threads[i], NULL);
    }

    for (int i = 0; i < g_config.threads; ++i) {
        pthread_join(cons_threads[i], NULL);
    }

    uint64_t t_end = timer_now_ns();
    double elapsed_sec = timer_diff_sec(t_start, t_end);

    uint64_t expected_total = (uint64_t)g_config.threads * (uint64_t)g_config.items;
    double throughput = (elapsed_sec > 0) ? ((double)g_total_consumed / elapsed_sec) : 0.0;
    double avg_latency_us = (g_total_consumed > 0) ? 
        ((double)g_total_latency_ns / (double)g_total_consumed / 1000.0) : 0.0;

    printf("\nSYNCHRONIZED QUEUE RESULTS:\n");
    printf("  Execution time:             %.4f s\n", elapsed_sec);
    printf("  Expected items:             %llu\n", (unsigned long long)expected_total);
    printf("  Actually produced:          %llu\n", (unsigned long long)g_total_produced);
    printf("  Actually consumed:          %llu\n", (unsigned long long)g_total_consumed);
    printf("  Checksum errors:            %llu\n", (unsigned long long)g_checksum_errors);
    printf("  Residual buffer count:      %zu\n", g_queue.buffer.count);
    printf("  Throughput:                 %.2f ops/sec\n", throughput);
    printf("  Average latency:            %.2f us\n", avg_latency_us);

    if (g_checksum_errors == 0 && g_total_consumed == expected_total && g_queue.buffer.count == 0) {
        printf("\n[STATUS] Verification: SUCCESS (0 errors, 100%% data integrity)\n");
    }

    free(prod_threads);
    free(cons_threads);
    free(pargs);
    sync_queue_destroy(&g_queue);
    return 0;
}
