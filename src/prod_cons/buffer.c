#include "buffer.h"
#include <stdlib.h>
#include <string.h>

int ring_buffer_init(ring_buffer_t *rb, size_t capacity) {
    if (!rb || capacity == 0) return -1;
    rb->data = (buffer_item_t *)calloc(capacity, sizeof(buffer_item_t));
    if (!rb->data) return -1;
    rb->capacity = capacity;
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
    return 0;
}

void ring_buffer_destroy(ring_buffer_t *rb) {
    if (!rb) return;
    if (rb->data) {
        free(rb->data);
        rb->data = NULL;
    }
    rb->capacity = 0;
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
}

bool ring_buffer_is_full(const ring_buffer_t *rb) {
    if (!rb) return true;
    return rb->count >= rb->capacity;
}

bool ring_buffer_is_empty(const ring_buffer_t *rb) {
    if (!rb) return true;
    return rb->count == 0;
}

bool ring_buffer_push_unsafe(ring_buffer_t *rb, buffer_item_t item) {
    if (!rb || ring_buffer_is_full(rb)) {
        return false;
    }

    rb->data[rb->tail] = item;
    rb->tail = (rb->tail + 1) % rb->capacity;
    rb->count++;
    return true;
}

bool ring_buffer_pop_unsafe(ring_buffer_t *rb, buffer_item_t *out_item) {
    if (!rb || ring_buffer_is_empty(rb)) {
        return false;
    }

    if (out_item) {
        *out_item = rb->data[rb->head];
    }
    rb->head = (rb->head + 1) % rb->capacity;
    rb->count--;
    return true;
}
