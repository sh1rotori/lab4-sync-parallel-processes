#ifndef MS_QUEUE_HPP
#define MS_QUEUE_HPP

#include <atomic>
#include <memory>
#include <utility>

/**
 * Michael-Scott Lock-Free FIFO Queue (1996).
 *
 * SYSTEMS ARCHITECTURE NOTE:
 * What makes this queue Lock-Free?
 * 1. Lock-Free Property:
 *    Guarantees system-wide progress. Even if some threads retry or starve,
 *    at least one thread is guaranteed to complete its operation in a bounded
 *    number of steps. No thread can block other threads if it crashes or is preempted.
 * 2. Helping Mechanism:
 *    Enqueue consists of two steps:
 *      Step 1: Swing cur_tail->next from nullptr to new_node.
 *      Step 2: Swing tail pointer to new_node.
 *    If thread A finishes Step 1 and is suspended by the OS, the queue is NOT blocked!
 *    Any other thread B that notices cur_tail->next != nullptr "helps" thread A by
 *    advancing tail via CAS before performing its own work.
 * 3. Sentinel / Dummy Node:
 *    Queue always contains at least one node (dummy).
 *    This avoids race conditions on an empty queue where head and tail would be nullptr.
 * 4. Memory Ordering:
 *    - Node stores before link: release semantics.
 *    - Pointer loads: acquire semantics.
 *    Guarantees happens-before relationship without heavy full memory barriers.
 */

template <typename T>
class MSQueue {
private:
    struct Node {
        T data;
        std::atomic<Node*> next;

        Node() : data(), next(nullptr) {}
        explicit Node(T value) : data(std::move(value)), next(nullptr) {}
    };

    std::atomic<Node*> head_;
    std::atomic<Node*> tail_;

public:
    MSQueue() {
        Node* dummy = new Node();
        head_.store(dummy, std::memory_order_relaxed);
        tail_.store(dummy, std::memory_order_relaxed);
    }

    ~MSQueue() {
        T val;
        while (dequeue(val)) {}
        delete head_.load(std::memory_order_relaxed);
    }

    MSQueue(const MSQueue&) = delete;
    MSQueue& operator=(const MSQueue&) = delete;

    /**
     * @brief Lock-free enqueue into tail.
     */
    void enqueue(T value) {
        Node* node = new Node(std::move(value));
        Node* cur_tail = nullptr;

        while (true) {
            cur_tail = tail_.load(std::memory_order_acquire);
            Node* cur_next = cur_tail->next.load(std::memory_order_acquire);

            if (cur_tail == tail_.load(std::memory_order_relaxed)) {
                if (cur_next == nullptr) {
                    /* Tail is at the end: try linking new node */
                    if (cur_tail->next.compare_exchange_weak(
                            cur_next, node, 
                            std::memory_order_release, 
                            std::memory_order_relaxed)) {
                        break;
                    }
                } else {
                    /* Tail is lagging: help advance tail */
                    tail_.compare_exchange_strong(
                        cur_tail, cur_next, 
                        std::memory_order_release, 
                        std::memory_order_relaxed);
                }
            }
        }

        /* Try advancing tail to the new node */
        tail_.compare_exchange_strong(
            cur_tail, node, 
            std::memory_order_release, 
            std::memory_order_relaxed);
    }

    /**
     * @brief Lock-free dequeue from head.
     * @return true if item dequeued, false if queue was empty.
     */
    bool dequeue(T& value) {
        while (true) {
            Node* cur_head = head_.load(std::memory_order_acquire);
            Node* cur_tail = tail_.load(std::memory_order_acquire);
            Node* next = cur_head->next.load(std::memory_order_acquire);

            if (cur_head == head_.load(std::memory_order_relaxed)) {
                if (cur_head == cur_tail) {
                    if (next == nullptr) {
                        return false; /* Queue is empty */
                    }
                    /* Tail is lagging: help advance */
                    tail_.compare_exchange_strong(
                        cur_tail, next, 
                        std::memory_order_release, 
                        std::memory_order_relaxed);
                } else {
                    if (next == nullptr) {
                        continue;
                    }
                    value = next->data;

                    /* Move head to next node */
                    if (head_.compare_exchange_weak(
                            cur_head, next, 
                            std::memory_order_release, 
                            std::memory_order_relaxed)) {
                        delete cur_head;
                        return true;
                    }
                }
            }
        }
    }
};

#endif /* MS_QUEUE_HPP */
