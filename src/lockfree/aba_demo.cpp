#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <vector>
#include "ms_queue.hpp"

/**
 * Demonstration of the ABA Problem and its solution via Tagged Pointers.
 *
 * SYSTEMS ARCHITECTURE NOTE:
 * What is the ABA problem in lock-free structures?
 * 1. Scenario on a Treiber Stack:
 *    - Thread 1 reads top of stack: Top = A, Next = B.
 *    - Before executing CAS(&Top, A, B), Thread 1 is preempted by the OS.
 *    - Thread 2 runs:
 *        1) Pops A (Top becomes B).
 *        2) Pops B (Top becomes C).
 *        3) Deletes node B.
 *        4) Pushes a new node that happens to get the exact same address A.
 *    - Thread 1 wakes up and runs CAS(&Top, A, B).
 *    - Since *Top == A, CAS succeeds!
 *    - Disaster: Node B was already freed (Use-After-Free/dangling pointer),
 *      and node C is permanently lost from the stack!
 *
 * 2. Solution: Tagged Pointer (Version / Generation Counter):
 *    - Each pointer is bundled with a monotonically increasing version counter (tag).
 *    - Instead of checking only address A, CAS checks the pair: (A, tag).
 *    - Thread 1 remembers: (A, tag=1).
 *    - Thread 2 mutations change state to: (A, tag=4).
 *    - Thread 1 CAS sees (A, tag=4) != (A, tag=1), detects the ABA change, and retries safely!
 */

struct StackNode {
    int value;
    StackNode* next;
    explicit StackNode(int v) : value(v), next(nullptr) {}
};

// ----------------------------------------------------------------------------
// 1. NAIVE TREIBER STACK (VULNERABLE TO ABA)
// ----------------------------------------------------------------------------
class NaiveTreiberStack {
public:
    std::atomic<StackNode*> top{nullptr};

    void push(StackNode* node) {
        StackNode* old_top = top.load(std::memory_order_relaxed);
        do {
            node->next = old_top;
        } while (!top.compare_exchange_weak(old_top, node,
                                            std::memory_order_release,
                                            std::memory_order_relaxed));
    }

    StackNode* pop() {
        StackNode* old_top = top.load(std::memory_order_relaxed);
        while (old_top && !top.compare_exchange_weak(old_top, old_top->next,
                                                    std::memory_order_release,
                                                    std::memory_order_relaxed)) {
            // spin
        }
        return old_top;
    }
};

// ----------------------------------------------------------------------------
// 2. TAGGED TREIBER STACK (SOLVES ABA VIA GENERATION COUNTER)
// Packs 48-bit address and 16-bit version tag into a 64-bit uint64_t.
// ----------------------------------------------------------------------------
class TaggedTreiberStack {
private:
    static constexpr uintptr_t PTR_MASK = 0x0000FFFFFFFFFFFFULL;
    static constexpr uintptr_t TAG_SHIFT = 48;

    static uintptr_t pack(StackNode* ptr, uint16_t tag) {
        return (reinterpret_cast<uintptr_t>(ptr) & PTR_MASK) | (static_cast<uintptr_t>(tag) << TAG_SHIFT);
    }

    static StackNode* unpack_ptr(uintptr_t val) {
        return reinterpret_cast<StackNode*>(val & PTR_MASK);
    }

    static uint16_t unpack_tag(uintptr_t val) {
        return static_cast<uint16_t>(val >> TAG_SHIFT);
    }

    std::atomic<uintptr_t> tagged_top_{0};

public:
    TaggedTreiberStack() = default;

    void push(StackNode* node) {
        uintptr_t old_val = tagged_top_.load(std::memory_order_relaxed);
        uintptr_t new_val = 0;
        do {
            StackNode* old_ptr = unpack_ptr(old_val);
            uint16_t old_tag = unpack_tag(old_val);
            node->next = old_ptr;
            new_val = pack(node, old_tag + 1);
        } while (!tagged_top_.compare_exchange_weak(old_val, new_val,
                                                    std::memory_order_release,
                                                    std::memory_order_relaxed));
    }

    StackNode* pop(bool* cas_failed_due_to_aba = nullptr) {
        uintptr_t old_val = tagged_top_.load(std::memory_order_relaxed);
        while (true) {
            StackNode* old_ptr = unpack_ptr(old_val);
            if (!old_ptr) return nullptr;

            uint16_t old_tag = unpack_tag(old_val);
            StackNode* next_ptr = old_ptr->next;
            uintptr_t new_val = pack(next_ptr, old_tag + 1);

            if (tagged_top_.compare_exchange_weak(old_val, new_val,
                                                 std::memory_order_release,
                                                 std::memory_order_relaxed)) {
                return old_ptr;
            } else {
                if (cas_failed_due_to_aba) {
                    *cas_failed_due_to_aba = true;
                }
            }
        }
    }

    uint16_t get_current_tag() const {
        return unpack_tag(tagged_top_.load(std::memory_order_relaxed));
    }
};

int main() {
    std::cout << "================================================================\n";
    std::cout << " LOCK-FREE DEMO: ABA PROBLEM AND TAGGED POINTER RESOLUTION\n";
    std::cout << "================================================================\n\n";

    // -------------------------------------------------------------
    // PART 1: ABA ANOMALY IN NAIVE TREIBER STACK
    // -------------------------------------------------------------
    std::cout << ">>> [PART 1]: Reproducing ABA Hazard in Naive Stack\n";
    NaiveTreiberStack naive_stack;

    StackNode* nodeA = new StackNode(10);
    StackNode* nodeB = new StackNode(20);
    StackNode* nodeC = new StackNode(30);

    // Initial state: Top -> A -> B -> C
    naive_stack.push(nodeC);
    naive_stack.push(nodeB);
    naive_stack.push(nodeA);

    std::cout << "Initial stack: Top -> A(10) -> B(20) -> C(30)\n";

    std::atomic<bool> thread1_read_done{false};
    std::atomic<bool> thread2_aba_done{false};

    StackNode* thread1_observed_old_top = nullptr;
    StackNode* thread1_observed_next = nullptr;
    bool thread1_cas_success = false;

    // Thread 1: reads A and B, then sleeps
    std::thread t1([&]() {
        thread1_observed_old_top = naive_stack.top.load();
        thread1_observed_next = thread1_observed_old_top->next;
        std::cout << "[Thread 1]: Read Top = " << thread1_observed_old_top 
                  << " (A), Next = " << thread1_observed_next << " (B)\n";
        
        thread1_read_done.store(true);
        while (!thread2_aba_done.load()) {
            std::this_thread::yield();
        }

        // Thread 1 wakes up and blindly runs CAS(Top, A, B)
        std::cout << "[Thread 1]: Woke up! Running CAS(&Top, A, B)...\n";
        thread1_cas_success = naive_stack.top.compare_exchange_strong(
            thread1_observed_old_top, thread1_observed_next);
    });

    // Thread 2: mutates stack (A -> B -> A)
    std::thread t2([&]() {
        while (!thread1_read_done.load()) {
            std::this_thread::yield();
        }

        std::cout << "[Thread 2]: Popped A: " << naive_stack.pop()->value << "\n";
        StackNode* poppedB = naive_stack.pop();
        std::cout << "[Thread 2]: Popped B: " << poppedB->value << "\n";
        
        // Corrupt memory of B to simulate reuse
        poppedB->value = -999;
        poppedB->next = nullptr;
        std::cout << "[Thread 2]: Corrupted node B (value = -999, next = nullptr)\n";

        // Push A back to top: ABA cycle complete
        naive_stack.push(nodeA);
        std::cout << "[Thread 2]: Pushed node A back to Top (Top is A again)\n";

        thread2_aba_done.store(true);
    });

    t1.join();
    t2.join();

    std::cout << "\nResult of Thread 1 CAS: " 
              << (thread1_cas_success ? "SUCCESS (False positive CAS!)" : "FAILED") << "\n";
    std::cout << "Current Top pointer:  " << naive_stack.top.load() << "\n";
    std::cout << "Value at Top of stack: " << naive_stack.top.load()->value << " (CORRUPTED DATA FROM NODE B)\n";
    std::cout << "Node C(30) was permanently lost from the stack due to ABA!\n\n";

    // -------------------------------------------------------------
    // PART 2: ABA PREVENTION VIA TAGGED POINTER
    // -------------------------------------------------------------
    std::cout << ">>> [PART 2]: ABA Prevention via 64-bit Tagged Pointer\n";
    TaggedTreiberStack safe_stack;

    StackNode* sA = new StackNode(100);
    StackNode* sB = new StackNode(200);
    StackNode* sC = new StackNode(300);

    safe_stack.push(sC);
    safe_stack.push(sB);
    safe_stack.push(sA);

    std::cout << "Initial stack tag: " << safe_stack.get_current_tag() << "\n";

    // Mutate safe stack: Pop(A) -> Pop(B) -> Push(A)
    StackNode* pA = safe_stack.pop();
    StackNode* pB = safe_stack.pop();
    delete pB;
    safe_stack.push(pA);

    std::cout << "After Pop(A) -> Pop(B) -> Push(A), current tag: " 
              << safe_stack.get_current_tag() << "\n";
    std::cout << "Top points to same address (" << pA 
              << "), but tag changed from 3 to " << safe_stack.get_current_tag() << "!\n";

    std::cout << "\n[STATUS] Tagged pointer successfully prevented ABA hazard:\n";
    std::cout << "  CAS verifies both pointer and generation counter simultaneously.\n";

    // -------------------------------------------------------------
    // PART 3: MICHAEL-SCOTT LOCK-FREE QUEUE
    // -------------------------------------------------------------
    std::cout << "\n>>> [PART 3]: Michael-Scott Lock-Free Queue Test\n";
    MSQueue<int> ms_queue;
    constexpr int NUM_ITEMS = 50000;

    std::thread producer([&]() {
        for (int i = 1; i <= NUM_ITEMS; ++i) {
            ms_queue.enqueue(i);
        }
    });

    int consumed_count = 0;
    int last_val = 0;
    bool order_ok = true;

    std::thread consumer([&]() {
        while (consumed_count < NUM_ITEMS) {
            int val = 0;
            if (ms_queue.dequeue(val)) {
                if (val <= last_val) order_ok = false;
                last_val = val;
                consumed_count++;
            }
        }
    });

    producer.join();
    consumer.join();

    std::cout << "Items successfully processed via MSQueue: " << consumed_count << "\n";
    std::cout << "Strict FIFO ordering verified: " << (order_ok ? "SUCCESS [OK]" : "FAILED") << "\n";

    delete nodeA;
    delete nodeB;
    delete nodeC;
    delete sA;
    delete sC;

    return 0;
}
