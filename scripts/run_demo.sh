#!/usr/bin/env bash
# ==============================================================================
# scripts/run_demo.sh
# Interactive CLI Showcase for Linux Concurrency Primitives
# ==============================================================================

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# Determine binary suffix (.exe on Windows, empty on Linux)
EXT=""
if [ -f "bin/prod_cons_sync.exe" ]; then
    EXT=".exe"
fi

COLOR_RESET="\033[0m"
COLOR_CYAN="\033[1;36m"
COLOR_GREEN="\033[1;32m"
COLOR_YELLOW="\033[1;33m"
COLOR_MAGENTA="\033[1;35m"

print_header() {
    clear 2>/dev/null || true
    echo -e "${COLOR_CYAN}======================================================================${COLOR_RESET}"
    echo -e "${COLOR_CYAN}  LINUX CONCURRENCY & SYNCHRONIZATION BENCHMARK SUITE${COLOR_RESET}"
    echo -e "${COLOR_CYAN}  POSIX Threads, Futexes, Spinlocks & Lock-Free Data Structures${COLOR_RESET}"
    echo -e "${COLOR_CYAN}======================================================================${COLOR_RESET}"
    echo ""
}

ensure_binaries() {
    if [ ! -f "bin/prod_cons_sync$EXT" ]; then
        echo -e "${COLOR_YELLOW}[!] Binaries not found. Running 'make all' first...${COLOR_RESET}"
        make all
        if [ -f "bin/prod_cons_sync.exe" ]; then
            EXT=".exe"
        fi
    fi
}

pause() {
    echo ""
    read -rp "Press Enter to return to menu..." _
}

while true; do
    print_header
    echo -e "Select scenario to execute:"
    echo -e "  ${COLOR_GREEN}1)${COLOR_RESET} Naive Producer-Consumer (Reproduce race condition & torn reads)"
    echo -e "  ${COLOR_GREEN}2)${COLOR_RESET} Synchronized Producer-Consumer (Mutex + 2 Condvars Mesa)"
    echo -e "  ${COLOR_GREEN}3)${COLOR_RESET} Readers-Writers: Readers Preference (Writer Starvation demo)"
    echo -e "  ${COLOR_GREEN}4)${COLOR_RESET} Readers-Writers: Writers Preference (Fair allocation)"
    echo -e "  ${COLOR_GREEN}5)${COLOR_RESET} Dining Philosophers (Dijkstra Resource Hierarchy)"
    echo -e "  ${COLOR_GREEN}6)${COLOR_RESET} Spinlock vs Mutex (Atomic flag + pause vs Futex contention)"
    echo -e "  ${COLOR_GREEN}7)${COLOR_RESET} Lock-Free Queue & ABA Problem (Treiber Stack + Tagged Pointer)"
    echo -e "  ${COLOR_GREEN}8)${COLOR_RESET} Run Full Automated Benchmark Suite (scripts/benchmark.py)"
    echo -e "  ${COLOR_GREEN}9)${COLOR_RESET} Clean & Recompile All Targets (make clean && make all)"
    echo -e "  ${COLOR_GREEN}0)${COLOR_RESET} Exit"
    echo ""
    read -rp "Enter choice [0-9]: " choice

    ensure_binaries

    case "$choice" in
        1)
            echo -e "\n${COLOR_MAGENTA}>>> Running Naive Producer-Consumer (4 threads, 25k items/thread)...${COLOR_RESET}"
            "./bin/prod_cons_naive$EXT" --threads 4 --buffer 16 --items 25000
            pause
            ;;
        2)
            echo -e "\n${COLOR_MAGENTA}>>> Running Synchronized Producer-Consumer (4 threads, 100k items/thread)...${COLOR_RESET}"
            "./bin/prod_cons_sync$EXT" --threads 4 --buffer 16 --items 100000
            pause
            ;;
        3)
            echo -e "\n${COLOR_MAGENTA}>>> Running Readers-Writers (Readers Preference)...${COLOR_RESET}"
            "./bin/rw_readers_pref$EXT" --threads 4
            pause
            ;;
        4)
            echo -e "\n${COLOR_MAGENTA}>>> Running Readers-Writers (Writers Preference)...${COLOR_RESET}"
            "./bin/rw_writers_pref$EXT" --threads 4
            pause
            ;;
        5)
            echo -e "\n${COLOR_MAGENTA}>>> Running Dining Philosophers (5 philosophers, 1000 meals)...${COLOR_RESET}"
            "./bin/philosophers$EXT" --threads 5 --items 1000
            pause
            ;;
        6)
            echo -e "\n${COLOR_MAGENTA}>>> Running Spinlock vs Mutex Contention Test (8 threads, 100k items)...${COLOR_RESET}"
            "./bin/spinlock_test$EXT" --threads 8 --items 100000
            pause
            ;;
        7)
            echo -e "\n${COLOR_MAGENTA}>>> Running Lock-Free Michael-Scott Queue & ABA Demo...${COLOR_RESET}"
            "./bin/aba_demo$EXT"
            pause
            ;;
        8)
            echo -e "\n${COLOR_MAGENTA}>>> Running Automated Benchmark Suite (Python)...${COLOR_RESET}"
            python3 scripts/benchmark.py
            pause
            ;;
        9)
            echo -e "\n${COLOR_MAGENTA}>>> Recompiling all targets...${COLOR_RESET}"
            make clean
            make all
            pause
            ;;
        0)
            echo "Exiting."
            exit 0
            ;;
        *)
            echo "Invalid option. Please try again."
            sleep 1
            ;;
    esac
done
