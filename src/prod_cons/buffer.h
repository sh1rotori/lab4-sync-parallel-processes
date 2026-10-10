#ifndef BUFFER_H
#define BUFFER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * Bounded circular (ring) buffer FIFO queue.
 *
 * SYSTEMS ARCHITECTURE NOTE:
 * 1. FIFO invariant: First-In, First-Out order.
 * 2. Modulo indexing:
 *      tail = (tail + 1) % capacity  (producer advances tail)
 *      head = (head + 1) % capacity  (consumer advances head)
 * 3. Shared critical state:
 *      'head', 'tail', 'count', and array 'data' are shared variables.
 *      Without synchronization:
 *      - Multiple producers write to the same data[tail] simultaneously (torn writes).
 *      - 'count++' and 'count--' suffer lost updates (not atomic).
 */

typedef struct {
    int producer_id;        /* Producer thread ID */
    int seq_num;            /* Message sequence number */
    uint64_t timestamp_ns;  /* Creation timestamp for latency measurement */
    uint32_t checksum;      /* Data integrity checksum */
} buffer_item_t;

typedef struct {
    buffer_item_t *data;    /* Dynamic array of items */
    size_t capacity;        /* Maximum buffer capacity */
    size_t head;            /* Consumer pop index */
    size_t tail;            /* Producer push index */
    size_t count;           /* Current number of items in buffer */
} ring_buffer_t;

int ring_buffer_init(ring_buffer_t *rb, size_t capacity);
void ring_buffer_destroy(ring_buffer_t *rb);
bool ring_buffer_is_full(const ring_buffer_t *rb);
bool ring_buffer_is_empty(const ring_buffer_t *rb);
bool ring_buffer_push_unsafe(ring_buffer_t *rb, buffer_item_t item);
bool ring_buffer_pop_unsafe(ring_buffer_t *rb, buffer_item_t *out_item);

static inline uint32_t buffer_item_calc_checksum(int prod_id, int seq) {
    return (uint32_t)((prod_id * 31337) ^ (seq * 7919));
}

#endif /* BUFFER_H */
