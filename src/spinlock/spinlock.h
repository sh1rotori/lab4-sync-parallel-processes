#ifndef SPINLOCK_H
#define SPINLOCK_H

#if defined(__cplusplus)
#include <atomic>
typedef std::atomic_flag spinlock_t;
#define SPINLOCK_INIT ATOMIC_FLAG_INIT
#else
#include <stdatomic.h>
typedef atomic_flag spinlock_t;
#define SPINLOCK_INIT ATOMIC_FLAG_INIT
#endif

/**
 * User-space Spinlock using atomic_flag (C11 / C++17).
 *
 * SYSTEMS ARCHITECTURE NOTE:
 * Why do we need the cpu_relax() / x86 PAUSE instruction?
 * 1. Prevents Pipeline Flush:
 *    During an active busy-wait loop, the CPU speculatively executes hundreds of reads.
 *    When another core writes to the flag, a memory order violation triggers an
 *    expensive CPU pipeline flush (~100-140 clock cycles).
 *    The 'pause' instruction introduces a tiny delay (10-140 cycles) and stabilizes
 *    the pipeline.
 * 2. Energy efficiency: reduces CPU power consumption and heat.
 * 3. Cache coherence (MESI protocol): reduces memory bus invalidation traffic.
 *
 * Memory Ordering:
 * - Lock uses memory_order_acquire: guarantees that no subsequent reads/writes inside
 *   the critical section can be reordered before the lock is acquired.
 * - Unlock uses memory_order_release: guarantees that all stores in the critical
 *   section become globally visible before the lock is cleared.
 */

static inline void cpu_relax(void) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #if defined(__GNUC__) || defined(__clang__)
        __builtin_ia32_pause();
    #else
        __asm__ volatile("pause" ::: "memory");
    #endif
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ volatile("yield" ::: "memory");
#else
    /* Fallback */
#endif
}

static inline void spinlock_init(spinlock_t *lock) {
#if defined(__cplusplus)
    lock->clear(std::memory_order_relaxed);
#else
    atomic_flag_clear_explicit(lock, memory_order_relaxed);
#endif
}

static inline void spinlock_lock(spinlock_t *lock) {
#if defined(__cplusplus)
    while (lock->test_and_set(std::memory_order_acquire)) {
        cpu_relax();
    }
#else
    while (atomic_flag_test_and_set_explicit(lock, memory_order_acquire)) {
        cpu_relax();
    }
#endif
}

static inline bool spinlock_trylock(spinlock_t *lock) {
#if defined(__cplusplus)
    return !lock->test_and_set(std::memory_order_acquire);
#else
    return !atomic_flag_test_and_set_explicit(lock, memory_order_acquire);
#endif
}

static inline void spinlock_unlock(spinlock_t *lock) {
#if defined(__cplusplus)
    lock->clear(std::memory_order_release);
#else
    atomic_flag_clear_explicit(lock, memory_order_release);
#endif
}

#endif /* SPINLOCK_H */
