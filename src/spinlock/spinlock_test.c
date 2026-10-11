#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include "spinlock.h"
#include "../common/timer.h"
#include "../common/cli_parser.h"

/**
 * Benchmark: User-space Spinlock vs Pthread Mutex (Linux Futex).
 *
 * SYSTEMS ARCHITECTURE NOTE:
 * When to choose Spinlock vs Mutex?
 * 1. Spinlock (busy-wait):
 *    - Pros: Minimal latency under low contention; zero syscalls, zero context switches.
 *    - Cons: Burns 100% CPU cycles; degrades heavily when thread count > physical cores;
 *      vulnerable to priority inversion and convoy effect.
 *    - Where to use: OS kernel interrupt handlers (where sleep is forbidden), or
 *      ultra-short critical sections (< 100 cycles) on isolated pinned CPU cores.
 * 2. Pthread Mutex (Linux Futex - Fast Userspace Mutex):
 *    - Two-tier design:
 *      * Fast-path: Handled entirely in user space via atomic CAS (cmpxchg).
 *        If there is no contention, zero syscalls are made!
 *      * Slow-path: If the lock is held, thread calls sys_futex(FUTEX_WAIT).
 *        The Linux kernel puts the thread to sleep, freeing the CPU core.
 *    - Pros: Energy efficient; stable under high contention.
 *    - Cons: Context switch wakeup latency (~1-5 microseconds).
 */

typedef struct {
    int thread_id;
    size_t iterations;
    spinlock_t *spinlock;
    pthread_mutex_t *mutex;
    uint64_t *shared_counter;
} bench_arg_t;

static cli_config_t g_config;

void *bench_spinlock_worker(void *arg) {
    bench_arg_t *b = (bench_arg_t *)arg;
    for (size_t i = 0; i < b->iterations; ++i) {
        spinlock_lock(b->spinlock);
        (*b->shared_counter)++;
        spinlock_unlock(b->spinlock);
    }
    return NULL;
}

void *bench_mutex_worker(void *arg) {
    bench_arg_t *b = (bench_arg_t *)arg;
    for (size_t i = 0; i < b->iterations; ++i) {
        pthread_mutex_lock(b->mutex);
        (*b->shared_counter)++;
        pthread_mutex_unlock(b->mutex);
    }
    return NULL;
}

int main(int argc, char **argv) {
    cli_config_init_defaults(&g_config);
    if (cli_parse_args(argc, argv, &g_config, argv[0]) != 0) {
        return 1;
    }

    int num_threads = g_config.threads;
    size_t iters_per_thread = g_config.items;
    uint64_t total_expected = (uint64_t)num_threads * iters_per_thread;

    printf("================================================================\n");
    printf(" OVERHEAD BENCHMARK: SPINLOCK VS PTHREAD_MUTEX\n");
    printf(" Worker threads:     %d\n", num_threads);
    printf(" Iterations/thread:  %zu (Total operations: %llu)\n", 
           iters_per_thread, (unsigned long long)total_expected);
    printf("================================================================\n\n");

    pthread_t *threads = malloc(sizeof(pthread_t) * (size_t)num_threads);
    bench_arg_t *args = malloc(sizeof(bench_arg_t) * (size_t)num_threads);

    /* -------------------------------------------------------------
     * TEST 1: USER-SPACE SPINLOCK
     * ------------------------------------------------------------- */
    spinlock_t my_spin;
    spinlock_init(&my_spin);
    uint64_t counter_spin = 0;

    uint64_t t0 = timer_now_ns();
    for (int i = 0; i < num_threads; ++i) {
        args[i].thread_id = i;
        args[i].iterations = iters_per_thread;
        args[i].spinlock = &my_spin;
        args[i].shared_counter = &counter_spin;
        pthread_create(&threads[i], NULL, bench_spinlock_worker, &args[i]);
    }
    for (int i = 0; i < num_threads; ++i) {
        pthread_join(threads[i], NULL);
    }
    uint64_t t1 = timer_now_ns();
    double time_spin = timer_diff_sec(t0, t1);
    double spin_throughput = (time_spin > 0) ? ((double)total_expected / time_spin) : 0.0;
    double spin_ns_op = (total_expected > 0) ? (time_spin * 1e9 / (double)total_expected) : 0.0;

    /* -------------------------------------------------------------
     * TEST 2: PTHREAD_MUTEX (FUTEX)
     * ------------------------------------------------------------- */
    pthread_mutex_t my_mutex;
    pthread_mutex_init(&my_mutex, NULL);
    uint64_t counter_mutex = 0;

    uint64_t t2 = timer_now_ns();
    for (int i = 0; i < num_threads; ++i) {
        args[i].thread_id = i;
        args[i].iterations = iters_per_thread;
        args[i].mutex = &my_mutex;
        args[i].shared_counter = &counter_mutex;
        pthread_create(&threads[i], NULL, bench_mutex_worker, &args[i]);
    }
    for (int i = 0; i < num_threads; ++i) {
        pthread_join(threads[i], NULL);
    }
    uint64_t t3 = timer_now_ns();
    double time_mutex = timer_diff_sec(t2, t3);
    double mutex_throughput = (time_mutex > 0) ? ((double)total_expected / time_mutex) : 0.0;
    double mutex_ns_op = (total_expected > 0) ? (time_mutex * 1e9 / (double)total_expected) : 0.0;

    pthread_mutex_destroy(&my_mutex);

    /* -------------------------------------------------------------
     * SUMMARY COMPARISON TABLE
     * ------------------------------------------------------------- */
    printf("----------------------------------------------------------------\n");
    printf("| Primitive        | Time (s)  | Throughput (ops/s) | Latency (ns) |\n");
    printf("----------------------------------------------------------------\n");
    printf("| SPINLOCK         | %9.4f | %18.2f | %12.2f |\n", time_spin, spin_throughput, spin_ns_op);
    printf("| PTHREAD_MUTEX    | %9.4f | %18.2f | %12.2f |\n", time_mutex, mutex_throughput, mutex_ns_op);
    printf("----------------------------------------------------------------\n\n");

    printf("CALCULATION INTEGRITY:\n");
    printf("  Spinlock final counter: %llu (expected: %llu) -> %s\n",
           (unsigned long long)counter_spin, (unsigned long long)total_expected,
           (counter_spin == total_expected) ? "OK [100%]" : "FAIL");
    printf("  Mutex final counter:    %llu (expected: %llu) -> %s\n",
           (unsigned long long)counter_mutex, (unsigned long long)total_expected,
           (counter_mutex == total_expected) ? "OK [100%]" : "FAIL");

    printf("\n[PERFORMANCE NOTE]\n");
    if (num_threads <= 2) {
        printf("  Low contention: Spinlock provides minimal overhead without kernel context switches.\n");
    } else {
        printf("  High contention: Cache line invalidations (MESI) increase spinlock latency;\n");
        printf("  Futex-based mutex scales better by sleeping contending threads.\n");
    }

    free(threads);
    free(args);
    return 0;
}
