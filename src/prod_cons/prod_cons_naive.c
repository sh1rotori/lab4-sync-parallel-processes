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
 * Naive Producer-Consumer without synchronization.
 * Demonstrates data race, lost updates, and memory corruption.
 *
 * SYSTEMS ARCHITECTURE NOTE:
 * 1. Data Race vs Race Condition:
 *    - Data Race: Two threads access the same memory location at the same time,
 *      at least one is write, and there is no synchronization (Undefined Behavior).
 *    - Race Condition: A high-level logic flaw where program correctness depends
 *      on thread scheduling order (e.g. check-then-act / TOCTOU).
 * 2. Lost Update:
 *    - 'count++' and 'count--' translate to 3 assembly instructions:
 *      LOAD -> ADD/SUB -> STORE.
 *      If thread gets preempted between LOAD and STORE, other thread's update is lost.
 * 3. Torn Reads / Torn Writes:
 *    - Writing to buffer_item without locks allows consumers to read half-written structs,
 *      detected by checksum mismatch.
 */

static ring_buffer_t g_buffer;
static cli_config_t g_config;

static volatile int64_t g_total_produced = 0;
static volatile int64_t g_total_consumed = 0;
static volatile int64_t g_checksum_errors = 0;

typedef struct {
    int thread_id;
    size_t items_to_produce;
} producer_arg_t;

typedef struct {
    int thread_id;
    size_t items_to_consume;
} consumer_arg_t;

void *naive_producer_thread(void *arg) {
    producer_arg_t *parg = (producer_arg_t *)arg;
    int id = parg->thread_id;
    size_t total = parg->items_to_produce;

    for (size_t seq = 0; seq < total; ++seq) {
        buffer_item_t item;
        item.producer_id = id;
        item.seq_num = (int)seq;
        item.timestamp_ns = timer_now_ns();
        item.checksum = buffer_item_calc_checksum(id, (int)seq);

        /* Unsafe write without mutex:
         * Multiple producers read the same tail and overwrite each other's data.
         */
        size_t idx = g_buffer.tail;
        g_buffer.data[idx] = item;
        g_buffer.tail = (idx + 1) % g_buffer.capacity;
        
        // Lost updates happen on non-atomic count increment
        g_buffer.count++;
        g_total_produced++;

        /* Small spin to provoke OS thread preemption */
        for (volatile int spin = 0; spin < 5; spin++);
    }

    return NULL;
}

void *naive_consumer_thread(void *arg) {
    consumer_arg_t *carg = (consumer_arg_t *)arg;
    size_t total = carg->items_to_consume;

    for (size_t i = 0; i < total; ++i) {
        /* Unsafe read without mutex:
         * Consumer reads while producer may be actively modifying data[head].
         */
        size_t idx = g_buffer.head;
        buffer_item_t item = g_buffer.data[idx];
        g_buffer.head = (idx + 1) % g_buffer.capacity;

        // Lost update on non-atomic decrement
        g_buffer.count--;
        g_total_consumed++;

        uint32_t expected = buffer_item_calc_checksum(item.producer_id, item.seq_num);
        if (item.checksum != expected) {
            // Memory corruption detected: torn read
            g_checksum_errors++;
        }

        for (volatile int spin = 0; spin < 5; spin++);
    }

    return NULL;
}

int main(int argc, char **argv) {
    cli_config_init_defaults(&g_config);
    if (cli_parse_args(argc, argv, &g_config, argv[0]) != 0) {
        return 1;
    }

    printf("================================================================\n");
    printf(" NAIVE PRODUCER-CONSUMER (NO SYNCHRONIZATION)\n");
    printf(" Producers: %d, Consumers: %d\n", g_config.threads, g_config.threads);
    printf(" Buffer capacity: %zu, Items per thread: %zu\n", g_config.buffer_size, g_config.items);
    printf("================================================================\n");

    if (ring_buffer_init(&g_buffer, g_config.buffer_size) != 0) {
        fprintf(stderr, "Buffer initialization failed\n");
        return 1;
    }

    pthread_t *prod_threads = malloc(sizeof(pthread_t) * (size_t)g_config.threads);
    pthread_t *cons_threads = malloc(sizeof(pthread_t) * (size_t)g_config.threads);
    producer_arg_t *pargs = malloc(sizeof(producer_arg_t) * (size_t)g_config.threads);
    consumer_arg_t *cargs = malloc(sizeof(consumer_arg_t) * (size_t)g_config.threads);

    uint64_t t_start = timer_now_ns();

    for (int i = 0; i < g_config.threads; ++i) {
        cargs[i].thread_id = i;
        cargs[i].items_to_consume = g_config.items;
        pthread_create(&cons_threads[i], NULL, naive_consumer_thread, &cargs[i]);

        pargs[i].thread_id = i;
        pargs[i].items_to_produce = g_config.items;
        pthread_create(&prod_threads[i], NULL, naive_producer_thread, &pargs[i]);
    }

    for (int i = 0; i < g_config.threads; ++i) {
        pthread_join(prod_threads[i], NULL);
        pthread_join(cons_threads[i], NULL);
    }

    uint64_t t_end = timer_now_ns();
    double elapsed_sec = timer_diff_sec(t_start, t_end);

    int64_t expected_total = (int64_t)g_config.threads * (int64_t)g_config.items;

    printf("\nTEST RESULTS:\n");
    printf("  Execution time:             %.4f s\n", elapsed_sec);
    printf("  Expected items:             %lld\n", (long long)expected_total);
    printf("  Actually produced:          %lld\n", (long long)g_total_produced);
    printf("  Actually consumed:          %lld\n", (long long)g_total_consumed);
    printf("  Checksum errors:            %lld (data corruption / torn reads)\n", (long long)g_checksum_errors);
    printf("  Residual buffer count:      %lld (expected 0, demonstrates lost updates)\n", (long long)g_buffer.count);

    if (g_checksum_errors > 0 || g_buffer.count != 0) {
        printf("\n[ANOMALY DETECTED] Race condition confirmed:\n");
        printf("  - Counter corrupted due to non-atomic RMW cycles (Lost Updates).\n");
        printf("  - Memory torn reads detected via checksum verification.\n");
    }

    free(prod_threads);
    free(cons_threads);
    free(pargs);
    free(cargs);
    ring_buffer_destroy(&g_buffer);
    return 0;
}
