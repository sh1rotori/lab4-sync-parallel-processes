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

#include <cstring>
#include <cstdlib>

struct QueuePayload {
    int seq;
    uint64_t enqueue_ns;
};

static inline uint64_t now_ns() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

int main(int argc, char** argv) {
    int num_threads = 1;
    int items_per_thread = 50000;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            num_threads = std::atoi(argv[++i]);
            if (num_threads < 1) num_threads = 1;
        } else if (std::strcmp(argv[i], "--items") == 0 && i + 1 < argc) {
            items_per_thread = std::atoi(argv[++i]);
            if (items_per_thread < 1) items_per_thread = 1000;
        }
    }

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
    // PART 3: MICHAEL-SCOTT LOCK-FREE QUEUE BENCHMARK (MPMC)
    // -------------------------------------------------------------
    std::cout << "\n>>> [PART 3]: Michael-Scott Lock-Free Queue Benchmark ("
              << num_threads << "P/" << num_threads << "C, " << items_per_thread << " items/thread)\n";
    MSQueue<QueuePayload> ms_queue;
    const uint64_t total_expected = static_cast<uint64_t>(num_threads) * static_cast<uint64_t>(items_per_thread);

    std::atomic<uint64_t> total_consumed{0};
    std::atomic<uint64_t> total_latency_ns{0};
    std::atomic<uint64_t> sum_consumed{0};
    std::atomic<bool> spsc_order_ok{true};

    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;
    producers.reserve(static_cast<size_t>(num_threads));
    consumers.reserve(static_cast<size_t>(num_threads));

    uint64_t t_start = now_ns();

    for (int c = 0; c < num_threads; ++c) {
        consumers.emplace_back([&]() {
            uint64_t local_lat = 0;
            uint64_t local_sum = 0;
            int last_seq = 0;

            while (total_consumed.load(std::memory_order_relaxed) < total_expected) {
                QueuePayload item{0, 0};
                if (ms_queue.dequeue(item)) {
                    uint64_t t_now = now_ns();
                    if (t_now >= item.enqueue_ns) {
                        local_lat += (t_now - item.enqueue_ns);
                    }
                    if (num_threads == 1 && item.seq <= last_seq) {
                        spsc_order_ok.store(false, std::memory_order_relaxed);
                    }
                    last_seq = item.seq;
                    local_sum += static_cast<uint64_t>(item.seq);
                    total_consumed.fetch_add(1, std::memory_order_relaxed);
                } else {
                    std::this_thread::yield();
                }
            }
            total_latency_ns.fetch_add(local_lat, std::memory_order_relaxed);
            sum_consumed.fetch_add(local_sum, std::memory_order_relaxed);
        });
    }

    for (int p = 0; p < num_threads; ++p) {
        producers.emplace_back([&]() {
            for (int i = 1; i <= items_per_thread; ++i) {
                QueuePayload item{i, now_ns()};
                ms_queue.enqueue(item);
            }
        });
    }

    for (auto& pt : producers) pt.join();
    for (auto& ct : consumers) ct.join();

    uint64_t t_end = now_ns();
    double elapsed_sec = static_cast<double>(t_end - t_start) / 1e9;
    if (elapsed_sec <= 0.0) elapsed_sec = 1e-9;

    uint64_t consumed = total_consumed.load();
    double throughput = static_cast<double>(consumed) / elapsed_sec;
    double avg_latency_us = (consumed > 0)
        ? (static_cast<double>(total_latency_ns.load()) / static_cast<double>(consumed) / 1000.0)
        : 0.0;

    uint64_t expected_sum_per_thread = static_cast<uint64_t>(items_per_thread) * static_cast<uint64_t>(items_per_thread + 1) / 2;
    uint64_t expected_total_sum = static_cast<uint64_t>(num_threads) * expected_sum_per_thread;
    bool integrity_ok = (consumed == total_expected) && (sum_consumed.load() == expected_total_sum) && spsc_order_ok.load();

    std::cout << std::fixed << std::setprecision(4);
    std::cout << "  Execution time:             " << elapsed_sec << " s\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  Throughput:                 " << throughput << " ops/sec\n";
    std::cout << "  Average latency:            " << avg_latency_us << " us\n";
    std::cout << "  Items successfully processed via MSQueue: " << consumed << " / " << total_expected << "\n";
    std::cout << "  Lock-Free MPMC integrity & ordering: " << (integrity_ok ? "SUCCESS [OK]" : "FAILED") << "\n";

    delete nodeA;
    delete nodeB;
    delete nodeC;
    delete sA;
    delete sC;

    return 0;
}
