#ifndef TIMER_H
#define TIMER_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <time.h>
#include <stdint.h>

/**
 * High-resolution monotonic timer using POSIX clock_gettime.
 *
 * SYSTEMS ARCHITECTURE NOTE:
 * Why CLOCK_MONOTONIC instead of CLOCK_REALTIME?
 * 1. CLOCK_REALTIME measures wall-clock calendar time (since Jan 1, 1970).
 *    It can jump backward or forward if system time changes (NTP sync, daylight saving).
 *    Never use it for benchmarks because intervals can become negative!
 * 2. CLOCK_MONOTONIC always moves forward steadily (since OS boot).
 *    It never jumps backward, making it the standard choice for benchmarks.
 * 3. CLOCK_PROCESS_CPUTIME_ID only counts CPU execution time.
 *    It ignores time spent sleeping on mutexes/condvars, so it cannot measure lock wait latency.
 */

static inline uint64_t timer_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline double timer_now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static inline uint64_t timer_diff_ns(uint64_t start_ns, uint64_t end_ns) {
    return (end_ns >= start_ns) ? (end_ns - start_ns) : 0;
}

static inline double timer_diff_sec(uint64_t start_ns, uint64_t end_ns) {
    return (double)timer_diff_ns(start_ns, end_ns) / 1e9;
}

#endif /* TIMER_H */
