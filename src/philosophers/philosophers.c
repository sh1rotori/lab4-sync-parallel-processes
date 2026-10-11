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
 * Dining Philosophers Problem.
 * Deadlock resolution using Dijkstra's Resource Hierarchy.
 *
 * SYSTEMS ARCHITECTURE NOTE:
 * 4 Coffman Conditions for Deadlock (1971):
 * 1. Mutual Exclusion: A fork can only be held by one philosopher.
 * 2. Hold and Wait: A philosopher holds one fork while waiting for another.
 * 3. No Preemption: A fork cannot be forcibly taken away.
 * 4. Circular Wait: P0 waits for P1, P1 waits for P2, ..., P(N-1) waits for P0.
 *
 * Dijkstra's Resource Hierarchy Solution:
 * - Define a strict global order on resources: R0 < R1 < ... < R(N-1).
 * - Every thread MUST acquire resources in increasing order of their IDs:
 *     first_fork  = min(left, right)
 *     second_fork = max(left, right)
 * - Why does this mathematically eliminate deadlock?
 *   It completely breaks Coffman's condition #4 (Circular Wait).
 *   The resource allocation graph becomes a Directed Acyclic Graph (DAG).
 *   The last philosopher (N-1) tries to acquire fork 0 BEFORE fork N-1, breaking symmetry.
 */

typedef struct {
    int id;
    int num_philosophers;
    int meals_to_eat;
    bool use_hierarchy;
    pthread_mutex_t *forks;
    uint64_t meals_eaten;
    uint64_t total_wait_ns;
} philosopher_t;

static cli_config_t g_config;

void *philosopher_thread(void *arg) {
    philosopher_t *p = (philosopher_t *)arg;
    int n = p->num_philosophers;

    int left = p->id;
    int right = (p->id + 1) % n;

    int first, second;

    if (p->use_hierarchy) {
        /* Dijkstra's resource hierarchy: always acquire lower index first */
        first = (left < right) ? left : right;
        second = (left < right) ? right : left;
    } else {
        /* Naive acquisition: always left then right (causes circular wait deadlock) */
        first = left;
        second = right;
    }

    for (int m = 0; m < p->meals_to_eat; ++m) {
        /* Thinking phase */
        usleep(50);

        /* Hungry phase -> Acquire forks */
        uint64_t wait_start = timer_now_ns();

        pthread_mutex_lock(&p->forks[first]);
        
        if (!p->use_hierarchy) {
            /* Small delay to provoke deadlock in naive mode */
            usleep(100);
        }

        pthread_mutex_lock(&p->forks[second]);

        uint64_t wait_end = timer_now_ns();
        p->total_wait_ns += timer_diff_ns(wait_start, wait_end);

        /* Eating phase */
        p->meals_eaten++;
        usleep(50);

        /* Release resources */
        pthread_mutex_unlock(&p->forks[second]);
        pthread_mutex_unlock(&p->forks[first]);
    }

    return NULL;
}

int main(int argc, char **argv) {
    cli_config_init_defaults(&g_config);
    if (cli_parse_args(argc, argv, &g_config, argv[0]) != 0) {
        return 1;
    }

    int n = (g_config.threads >= 2) ? g_config.threads : 5;
    int meals = (g_config.items > 0) ? (int)g_config.items : 1000;
    if (meals > 5000) meals = 5000;
    bool use_hierarchy = !g_config.no_sync;

    printf("================================================================\n");
    printf(" DINING PHILOSOPHERS PROBLEM\n");
    printf(" Philosophers: %d, Meals per philosopher: %d\n", n, meals);
    printf(" Algorithm: %s\n", use_hierarchy ? 
           "Dijkstra Resource Hierarchy (Deadlock-Free)" : 
           "Naive Acquisition (Prone to Deadlock)");
    printf("================================================================\n");

    pthread_mutex_t *forks = malloc(sizeof(pthread_mutex_t) * (size_t)n);
    philosopher_t *philosophers = malloc(sizeof(philosopher_t) * (size_t)n);
    pthread_t *threads = malloc(sizeof(pthread_t) * (size_t)n);

    for (int i = 0; i < n; ++i) {
        pthread_mutex_init(&forks[i], NULL);
    }

    uint64_t t_start = timer_now_ns();

    for (int i = 0; i < n; ++i) {
        philosophers[i].id = i;
        philosophers[i].num_philosophers = n;
        philosophers[i].meals_to_eat = meals;
        philosophers[i].use_hierarchy = use_hierarchy;
        philosophers[i].forks = forks;
        philosophers[i].meals_eaten = 0;
        philosophers[i].total_wait_ns = 0;
        pthread_create(&threads[i], NULL, philosopher_thread, &philosophers[i]);
    }

    for (int i = 0; i < n; ++i) {
        pthread_join(threads[i], NULL);
    }

    uint64_t t_end = timer_now_ns();
    double elapsed_sec = timer_diff_sec(t_start, t_end);

    uint64_t total_meals = 0;
    uint64_t total_wait = 0;
    for (int i = 0; i < n; ++i) {
        total_meals += philosophers[i].meals_eaten;
        total_wait += philosophers[i].total_wait_ns;
    }

    double avg_wait_us = (total_meals > 0) ? ((double)total_wait / (double)total_meals / 1000.0) : 0.0;

    printf("\nTEST RESULTS:\n");
    printf("  Execution time:             %.4f s\n", elapsed_sec);
    printf("  Expected total meals:       %d\n", n * meals);
    printf("  Actually completed meals:   %llu\n", (unsigned long long)total_meals);
    printf("  Average fork wait time:     %.2f us\n", avg_wait_us);

    for (int i = 0; i < n; ++i) {
        printf("    Philosopher %d ate: %llu meals\n", i, (unsigned long long)philosophers[i].meals_eaten);
    }

    if (total_meals == (uint64_t)(n * meals)) {
        printf("\n[STATUS] Deadlock-free execution confirmed:\n");
        printf("  Dijkstra resource hierarchy successfully eliminated circular wait.\n");
    }

    for (int i = 0; i < n; ++i) {
        pthread_mutex_destroy(&forks[i]);
    }
    free(forks);
    free(philosophers);
    free(threads);
    return 0;
}
