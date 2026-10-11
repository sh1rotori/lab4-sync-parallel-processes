#!/usr/bin/env python3
"""
Automated parallel synchronization benchmark suite.
Measures throughput and latency across 1, 2, 4, 8, and 16 threads,
exports metrics to results/data.csv, and generates plots in results/plots/.
"""

import os
import sys
import subprocess
import re
import csv
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BIN_DIR = os.path.join(PROJECT_ROOT, "bin")
RESULTS_DIR = os.path.join(PROJECT_ROOT, "results")
PLOTS_DIR = os.path.join(RESULTS_DIR, "plots")
CSV_PATH = os.path.join(RESULTS_DIR, "data.csv")

THREAD_COUNTS = [1, 2, 4, 8, 16]
ITEMS_COUNT = 100000

def get_binary_path(name):
    # Support both Linux ELF and Windows PE (.exe)
    base = os.path.join(BIN_DIR, name)
    if os.path.isfile(base):
        return base
    if os.path.isfile(base + ".exe"):
        return base + ".exe"
    raise FileNotFoundError(f"Binary '{name}' not found in {BIN_DIR}. Run 'make all' first.")

def run_cmd(cmd):
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, encoding="utf-8", errors="replace")
    if result.returncode != 0:
        print(f"Command failed: {' '.join(cmd)}\n{result.stderr}")
    return result.stdout or ""

def run_spinlock_test(threads):
    bin_path = get_binary_path("spinlock_test")
    out = run_cmd([bin_path, "--threads", str(threads), "--items", str(ITEMS_COUNT)])
    
    # Парсинг таблицы
    # | SPINLOCK         |    0.0377 |        10604510.10 |        94.30 |
    # | PTHREAD_MUTEX    |    0.0089 |        44762757.39 |        22.34 |
    spin_match = re.search(r"\|\s*SPINLOCK\s*\|\s*([\d\.]+)\s*\|\s*([\d\.]+)\s*\|\s*([\d\.]+)\s*\|", out)
    mutex_match = re.search(r"\|\s*PTHREAD_MUTEX\s*\|\s*([\d\.]+)\s*\|\s*([\d\.]+)\s*\|\s*([\d\.]+)\s*\|", out)

    res = []
    if spin_match:
        res.append({
            "threads": threads,
            "benchmark": "counter_increment",
            "sync_primitive": "Spinlock",
            "throughput_ops_sec": float(spin_match.group(2)),
            "latency_ns": float(spin_match.group(3)),
            "duration_sec": float(spin_match.group(1))
        })
    if mutex_match:
        res.append({
            "threads": threads,
            "benchmark": "counter_increment",
            "sync_primitive": "Pthread_Mutex",
            "throughput_ops_sec": float(mutex_match.group(2)),
            "latency_ns": float(mutex_match.group(3)),
            "duration_sec": float(mutex_match.group(1))
        })
    return res

def run_prod_cons_sync(threads):
    bin_path = get_binary_path("prod_cons_sync")
    # buffer=16, items=50000
    out = run_cmd([bin_path, "--threads", str(threads), "--buffer", "16", "--items", str(ITEMS_COUNT // 2)])
    
    # Пропускная способность:     455155.78 ops/sec
    # Средняя задержка (latency): 11.20 мкс
    # Время выполнения:           0.0220 сек
    tp_match = re.search(r"(?:Throughput|Пропускная способность):\s*([\d\.]+)\s*ops/sec", out)
    lat_match = re.search(r"(?:Average latency|Средняя задержка \(latency\)):\s*([\d\.]+)\s*(?:us|мкс)", out)
    time_match = re.search(r"(?:Execution time|Время выполнения):\s*([\d\.]+)\s*(?:s|сек)", out)

    if tp_match and lat_match and time_match:
        return [{
            "threads": threads,
            "benchmark": "bounded_queue",
            "sync_primitive": "Mutex+2Condvars",
            "throughput_ops_sec": float(tp_match.group(1)),
            "latency_ns": float(lat_match.group(1)) * 1000.0, # мкс в нс
            "duration_sec": float(time_match.group(1))
        }]
    return []

def run_lockfree_msqueue(threads):
    bin_path = get_binary_path("aba_demo")
    out = run_cmd([bin_path, "--threads", str(threads), "--items", str(ITEMS_COUNT // 2)])

    tp_match = re.search(r"Throughput:\s*([\d\.]+)\s*ops/sec", out)
    lat_match = re.search(r"Average latency:\s*([\d\.]+)\s*us", out)
    time_match = re.search(r"Execution time:\s*([\d\.]+)\s*s", out)

    if tp_match and lat_match and time_match:
        return [{
            "threads": threads,
            "benchmark": "lockfree_queue",
            "sync_primitive": "LockFree_MSQueue",
            "throughput_ops_sec": float(tp_match.group(1)),
            "latency_ns": float(lat_match.group(1)) * 1000.0,
            "duration_sec": float(time_match.group(1))
        }]
    return []

def main():
    os.makedirs(RESULTS_DIR, exist_ok=True)
    os.makedirs(PLOTS_DIR, exist_ok=True)

    print("=================================================================")
    print(" RUNNING SYNCHRONIZATION BENCHMARK SUITE (1, 2, 4, 8, 16 THREADS)")
    print("=================================================================")

    all_records = []

    for t in THREAD_COUNTS:
        print(f"\n>>> Benchmarking with thread count: {t} ...")
        # 1. Spinlock vs Mutex
        spin_res = run_spinlock_test(t)
        all_records.extend(spin_res)
        for r in spin_res:
            print(f"    [{r['sync_primitive']:16}] Throughput: {r['throughput_ops_sec']:12.2f} ops/s, Latency: {r['latency_ns']:8.2f} ns")

        # 2. Prod-Cons Sync (Mutex + 2 Condvars)
        pc_res = run_prod_cons_sync(t)
        all_records.extend(pc_res)
        for r in pc_res:
            print(f"    [{r['sync_primitive']:16}] Throughput: {r['throughput_ops_sec']:12.2f} ops/s, Latency: {r['latency_ns']:8.2f} ns")

        # 3. Lock-Free Michael-Scott Queue (CAS)
        lf_res = run_lockfree_msqueue(t)
        all_records.extend(lf_res)
        for r in lf_res:
            print(f"    [{r['sync_primitive']:16}] Throughput: {r['throughput_ops_sec']:12.2f} ops/s, Latency: {r['latency_ns']:8.2f} ns")

    # Save to CSV
    fieldnames = ["threads", "benchmark", "sync_primitive", "throughput_ops_sec", "latency_ns", "duration_sec"]
    with open(CSV_PATH, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for r in all_records:
            writer.writerow(r)
    print(f"\n[OK] Metrics saved to: {CSV_PATH}")

    # Generate Matplotlib charts
    generate_plots(all_records)

def generate_plots(records):
    data_by_prim = {}
    for r in records:
        prim = r["sync_primitive"]
        if prim not in data_by_prim:
            data_by_prim[prim] = {"threads": [], "throughput": [], "latency": []}
        data_by_prim[prim]["threads"].append(r["threads"])
        data_by_prim[prim]["throughput"].append(r["throughput_ops_sec"])
        data_by_prim[prim]["latency"].append(r["latency_ns"])

    # 1. Throughput Plot
    plt.figure(figsize=(9, 5.5), dpi=300)
    for prim, vals in data_by_prim.items():
        if prim in ["Spinlock", "Pthread_Mutex"]:
            plt.plot(vals["threads"], [v / 1e6 for v in vals["throughput"]], marker='o', linewidth=2.2, label=f"{prim} (M ops/s)")
    plt.title("Throughput Comparison: TAS Spinlock vs Pthread Mutex", fontsize=13, fontweight='bold', pad=12)
    plt.xlabel("Concurrent Threads", fontsize=11)
    plt.ylabel("Throughput (Million ops/sec)", fontsize=11)
    plt.xticks(THREAD_COUNTS)
    plt.grid(True, linestyle="--", alpha=0.6)
    plt.legend(frameon=True, facecolor="#f8f9fa", edgecolor="#ced4da")
    plt.tight_layout()
    plot_tp_path = os.path.join(PLOTS_DIR, "throughput.png")
    plt.savefig(plot_tp_path)
    plt.close()
    print(f"[OK] Generated plot: {plot_tp_path}")

    # 2. Latency Plot
    plt.figure(figsize=(9, 5.5), dpi=300)
    for prim, vals in data_by_prim.items():
        if prim in ["Spinlock", "Pthread_Mutex"]:
            plt.plot(vals["threads"], vals["latency"], marker='s', linewidth=2.2, label=f"{prim}")
    plt.title("Lock Acquisition Latency vs Thread Contention", fontsize=13, fontweight='bold', pad=12)
    plt.xlabel("Concurrent Threads", fontsize=11)
    plt.ylabel("Average Operation Latency (ns)", fontsize=11)
    plt.xticks(THREAD_COUNTS)
    plt.grid(True, linestyle="--", alpha=0.6)
    plt.legend(frameon=True, facecolor="#f8f9fa", edgecolor="#ced4da")
    plt.tight_layout()
    plot_lat_path = os.path.join(PLOTS_DIR, "latency.png")
    plt.savefig(plot_lat_path)
    plt.close()
    print(f"[OK] Generated plot: {plot_lat_path}")

    # 3. Queue Contention & Scalability Plot: Mutex+2Condvars vs Lock-Free MSQueue
    plt.figure(figsize=(9, 5.5), dpi=300)
    if "Mutex+2Condvars" in data_by_prim:
        pc_vals = data_by_prim["Mutex+2Condvars"]
        plt.plot(pc_vals["threads"], [v / 1e3 for v in pc_vals["throughput"]],
                 marker='^', color='#2ca02c', linewidth=2.2, label="Mutex + 2 Condvars (Bounded Ring Buffer)")
    if "LockFree_MSQueue" in data_by_prim:
        lf_vals = data_by_prim["LockFree_MSQueue"]
        plt.plot(lf_vals["threads"], [v / 1e3 for v in lf_vals["throughput"]],
                 marker='D', color='#d62728', linewidth=2.2, label="Lock-Free Michael-Scott Queue (CAS)")
    plt.title("MPMC Queue Scalability: Lock-Free MSQueue vs Mutex+2Condvars", fontsize=13, fontweight='bold', pad=12)
    plt.xlabel("Producer/Consumer Thread Pairs (N Producers + N Consumers)", fontsize=11)
    plt.ylabel("Throughput (Thousand items/sec)", fontsize=11)
    plt.xticks(THREAD_COUNTS)
    plt.grid(True, linestyle="--", alpha=0.6)
    plt.legend(frameon=True, facecolor="#f8f9fa", edgecolor="#ced4da")
    plt.tight_layout()
    plot_cont_path = os.path.join(PLOTS_DIR, "contention.png")
    plt.savefig(plot_cont_path)
    plt.close()
    print(f"[OK] Generated plot: {plot_cont_path}")

if __name__ == "__main__":
    main()
