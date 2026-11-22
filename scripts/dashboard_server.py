#!/usr/bin/env python3
"""
Zero-dependency offline web dashboard server for lab4_sync.
Uses ONLY Python 3 standard library modules (no pip install required).

Features:
- Serves self-contained Russian presentation frontend from dashboard/
- Reads results/data.csv and computes KPIs/verdicts
- Executes compiled C/C++ synchronization binaries safely on demand
- Proxies grounded prompts to Gemini 2.5 Flash via urllib.request with automatic
  rule-based offline fallback (never exposes API keys to logs or UI)
"""

import argparse
import copy
import csv
import json
import mimetypes
import os
import platform
import re
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
BIN_DIR = PROJECT_ROOT / "bin"
RESULTS_DIR = PROJECT_ROOT / "results"
CSV_PATH = RESULTS_DIR / "data.csv"
DASHBOARD_DIR = (PROJECT_ROOT / "dashboard").resolve()
ENV_PATH = PROJECT_ROOT / ".env"

ALLOWED_SCENARIOS = {
    "suite",
    "spinlock_test",
    "prod_cons_sync",
    "aba_demo",
    "philosophers",
}
TRACKED_BINARIES = ("spinlock_test", "prod_cons_sync", "aba_demo", "philosophers")
THREAD_COUNTS = [1, 2, 4, 8, 16]
GEMINI_ENDPOINT = (
    "https://generativelanguage.googleapis.com/v1beta/models/"
    "gemini-2.5-flash:generateContent"
)

_INVALID_KEY_PLACEHOLDERS = {
    "",
    "your_api_key_here",
    "your-api-key-here",
    "changeme",
    "placeholder",
    "none",
    "null",
}


def _load_gemini_api_key() -> str:
    """Load GEMINI_API_KEY from environment or .env without exposing it."""
    env_val = os.environ.get("GEMINI_API_KEY", "").strip()
    if env_val and env_val.lower() not in _INVALID_KEY_PLACEHOLDERS:
        return env_val

    if ENV_PATH.is_file():
        try:
            with ENV_PATH.open("r", encoding="utf-8") as fh:
                for raw_line in fh:
                    line = raw_line.strip()
                    if not line or line.startswith("#") or "=" not in line:
                        continue
                    key_name, val = line.split("=", 1)
                    if key_name.strip() == "GEMINI_API_KEY":
                        cleaned = val.strip().strip('"').strip("'").strip()
                        if cleaned and cleaned.lower() not in _INVALID_KEY_PLACEHOLDERS:
                            return cleaned
        except OSError:
            return ""
    return ""


def get_binary_path(name: str) -> Path:
    """Resolve compiled binary path for Linux ELF or Windows PE (.exe)."""
    if name not in TRACKED_BINARIES:
        raise ValueError(f"Неподдерживаемый бинарный файл: {name}")
    base = BIN_DIR / name
    if base.is_file():
        return base
    exe_path = BIN_DIR / f"{name}.exe"
    if exe_path.is_file():
        return exe_path
    raise FileNotFoundError(
        f"Бинарный файл '{name}' не найден в {BIN_DIR}. Сначала выполните 'make all'."
    )


def get_binary_availability() -> dict:
    """Check availability of all dashboard-supported compiled binaries."""
    status = {}
    for name in TRACKED_BINARIES:
        try:
            get_binary_path(name)
            status[name] = True
        except FileNotFoundError:
            status[name] = False
    return status


def clamp_int(value, default: int, minimum: int, maximum: int) -> int:
    """Safely parse and clamp an integer parameter."""
    try:
        parsed = int(value)
    except (TypeError, ValueError):
        return default
    return max(minimum, min(maximum, parsed))


def run_binary(args: list, timeout: int = 60) -> tuple:
    """Execute a compiled binary safely without a shell."""
    proc = subprocess.run(
        args,
        cwd=str(PROJECT_ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
        shell=False,
    )
    return proc.returncode, proc.stdout or "", proc.stderr or ""


def parse_spinlock_output(stdout: str, threads: int) -> list:
    records = []
    spin_match = re.search(
        r"\|\s*SPINLOCK\s*\|\s*([\d\.]+)\s*\|\s*([\d\.]+)\s*\|\s*([\d\.]+)\s*\|",
        stdout,
    )
    mutex_match = re.search(
        r"\|\s*PTHREAD_MUTEX\s*\|\s*([\d\.]+)\s*\|\s*([\d\.]+)\s*\|\s*([\d\.]+)\s*\|",
        stdout,
    )
    if spin_match:
        records.append(
            {
                "threads": threads,
                "benchmark": "counter_increment",
                "sync_primitive": "Spinlock",
                "throughput_ops_sec": float(spin_match.group(2)),
                "latency_ns": float(spin_match.group(3)),
                "duration_sec": float(spin_match.group(1)),
            }
        )
    if mutex_match:
        records.append(
            {
                "threads": threads,
                "benchmark": "counter_increment",
                "sync_primitive": "Pthread_Mutex",
                "throughput_ops_sec": float(mutex_match.group(2)),
                "latency_ns": float(mutex_match.group(3)),
                "duration_sec": float(mutex_match.group(1)),
            }
        )
    return records


def parse_prod_cons_output(stdout: str, threads: int) -> list:
    tp_match = re.search(
        r"(?:Throughput|Пропускная способность):\s*([\d\.]+)\s*ops/sec", stdout
    )
    lat_match = re.search(
        r"(?:Average latency|Средняя задержка \(latency\)):\s*([\d\.]+)\s*(?:us|мкс)",
        stdout,
    )
    time_match = re.search(
        r"(?:Execution time|Время выполнения):\s*([\d\.]+)\s*(?:s|сек)", stdout
    )
    if tp_match and lat_match and time_match:
        return [
            {
                "threads": threads,
                "benchmark": "bounded_queue",
                "sync_primitive": "Mutex+2Condvars",
                "throughput_ops_sec": float(tp_match.group(1)),
                "latency_ns": float(lat_match.group(1)) * 1000.0,
                "duration_sec": float(time_match.group(1)),
            }
        ]
    return []


def parse_aba_demo_output(stdout: str, threads: int) -> list:
    tp_match = re.search(r"Throughput:\s*([\d\.]+)\s*ops/sec", stdout)
    lat_match = re.search(r"Average latency:\s*([\d\.]+)\s*us", stdout)
    time_match = re.search(r"Execution time:\s*([\d\.]+)\s*s", stdout)
    if tp_match and lat_match and time_match:
        return [
            {
                "threads": threads,
                "benchmark": "lockfree_queue",
                "sync_primitive": "LockFree_MSQueue",
                "throughput_ops_sec": float(tp_match.group(1)),
                "latency_ns": float(lat_match.group(1)) * 1000.0,
                "duration_sec": float(time_match.group(1)),
            }
        ]
    return []


def parse_philosophers_output(stdout: str, threads: int) -> dict:
    time_match = re.search(r"Execution time:\s*([\d\.]+)\s*s", stdout)
    meals_match = re.search(r"Actually completed meals:\s*(\d+)", stdout)
    wait_match = re.search(r"Average fork wait time:\s*([\d\.]+)\s*us", stdout)
    deadlock = "[DEADLOCK DETECTED]" in stdout
    return {
        "threads": threads,
        "benchmark": "dining_philosophers",
        "duration_sec": float(time_match.group(1)) if time_match else None,
        "completed_meals": int(meals_match.group(1)) if meals_match else None,
        "avg_wait_us": float(wait_match.group(1)) if wait_match else None,
        "deadlock_detected": deadlock,
    }


def load_csv_records() -> list:
    """Read benchmark records from results/data.csv."""
    if not CSV_PATH.is_file():
        return []
    records = []
    try:
        with CSV_PATH.open("r", newline="", encoding="utf-8") as fh:
            reader = csv.DictReader(fh)
            for row in reader:
                try:
                    records.append(
                        {
                            "threads": int(row["threads"]),
                            "benchmark": row["benchmark"].strip(),
                            "sync_primitive": row["sync_primitive"].strip(),
                            "throughput_ops_sec": float(row["throughput_ops_sec"]),
                            "latency_ns": float(row["latency_ns"]),
                            "duration_sec": float(row["duration_sec"]),
                        }
                    )
                except (KeyError, TypeError, ValueError):
                    continue
    except OSError:
        return []
    return records


def save_csv_records(records: list) -> None:
    """Persist benchmark suite records to results/data.csv."""
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    fieldnames = [
        "threads",
        "benchmark",
        "sync_primitive",
        "throughput_ops_sec",
        "latency_ns",
        "duration_sec",
    ]
    with CSV_PATH.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        for rec in records:
            writer.writerow({k: rec[k] for k in fieldnames})


def group_series(records: list) -> dict:
    """Group CSV records by sync_primitive ordered by thread count."""
    series = {}
    for rec in records:
        prim = rec["sync_primitive"]
        series.setdefault(prim, []).append(rec)
    for prim in series:
        series[prim].sort(key=lambda item: item["threads"])
    return series


def compute_kpis(records: list, last_philosophers: dict = None) -> dict:
    """Compute high-level KPIs and systems verdicts in Russian."""
    kpis = {
        "throughput_winner": None,
        "lowest_latency": None,
        "msqueue_speedup": None,
        "bottleneck_alert": {
            "severity": "info",
            "title": "Нет данных бенчмарка",
            "detail": "Запустите бенчмарк или загрузите results/data.csv для анализа метрик конкуренции.",
        },
    }

    if not records:
        if last_philosophers and last_philosophers.get("deadlock_detected"):
            kpis["bottleneck_alert"] = {
                "severity": "danger",
                "title": "Обнаружен Deadlock (Философы)",
                "detail": "Возникло круговое ожидание в наивном режиме --no-sync (watchdog-таймаут >500 мс).",
            }
        return kpis

    best_tp = max(records, key=lambda r: r["throughput_ops_sec"])
    kpis["throughput_winner"] = {
        "primitive": best_tp["sync_primitive"],
        "throughput_ops_sec": best_tp["throughput_ops_sec"],
        "threads": best_tp["threads"],
        "benchmark": best_tp["benchmark"],
    }

    best_lat = min(records, key=lambda r: r["latency_ns"])
    kpis["lowest_latency"] = {
        "primitive": best_lat["sync_primitive"],
        "latency_ns": best_lat["latency_ns"],
        "threads": best_lat["threads"],
        "benchmark": best_lat["benchmark"],
    }

    by_prim = group_series(records)
    msq_by_t = {r["threads"]: r for r in by_prim.get("LockFree_MSQueue", [])}
    blk_by_t = {r["threads"]: r for r in by_prim.get("Mutex+2Condvars", [])}
    shared_threads = sorted(set(msq_by_t.keys()) & set(blk_by_t.keys()))
    if shared_threads:
        max_t = shared_threads[-1]
        blk_tp = blk_by_t[max_t]["throughput_ops_sec"]
        msq_tp = msq_by_t[max_t]["throughput_ops_sec"]
        ratio = (msq_tp / blk_tp) if blk_tp > 0 else 0.0
        kpis["msqueue_speedup"] = {
            "ratio": round(ratio, 2),
            "threads": max_t,
            "msqueue_ops_sec": msq_tp,
            "blocking_ops_sec": blk_tp,
        }

    # Priority bottleneck check: recent Dining Philosophers deadlock
    if last_philosophers and last_philosophers.get("deadlock_detected"):
        kpis["bottleneck_alert"] = {
            "severity": "danger",
            "title": "Обнаружен Deadlock (Круговое ожидание)",
            "detail": (
                f"Обедающие философы ({last_philosophers.get('threads', '?')} потоков) попали в "
                "Hold-and-Wait в режиме --no-sync. Отключите флаг для иерархии Дейкстры."
            ),
        }
        return kpis

    spin_rows = by_prim.get("Spinlock", [])
    mutex_rows = by_prim.get("Pthread_Mutex", [])
    if spin_rows and mutex_rows:
        spin_low = spin_rows[0]
        spin_high = spin_rows[-1]
        mutex_high = mutex_rows[-1]
        drop_factor = (
            spin_low["throughput_ops_sec"] / spin_high["throughput_ops_sec"]
            if spin_high["throughput_ops_sec"] > 0
            else 0.0
        )
        if spin_high["throughput_ops_sec"] < mutex_high["throughput_ops_sec"]:
            adv = (
                mutex_high["throughput_ops_sec"] / spin_high["throughput_ops_sec"]
                if spin_high["throughput_ops_sec"] > 0
                else 0.0
            )
            kpis["bottleneck_alert"] = {
                "severity": "warning",
                "title": f"Cache-Bouncing Spinlock ({spin_high['threads']} потоков)",
                "detail": (
                    f"Spinlock упал в {drop_factor:.1f}x от {spin_low['threads']}T; "
                    f"Futex Pthread_Mutex быстрее в {adv:.1f}x при {spin_high['threads']} потоках."
                ),
            }
        else:
            kpis["bottleneck_alert"] = {
                "severity": "ok",
                "title": "Режим низкой конкуренции",
                "detail": (
                    f"Spinlock сохраняет эффективность до {spin_high['threads']} потоков "
                    f"(средняя задержка {spin_high['latency_ns']:.1f} нс)."
                ),
            }
    return kpis


class BenchmarkManager:
    """Thread-safe runner for compiling/running C/C++ benchmarks with live status."""

    def __init__(self):
        self._lock = threading.Lock()
        self._state = {
            "running": False,
            "job_id": 0,
            "scenario": None,
            "step": 0,
            "total_steps": 0,
            "message": "Ожидание",
            "raw_stdout": "",
            "parsed_metrics": [],
            "last_philosophers": None,
            "error": None,
            "started_at": None,
            "finished_at": None,
        }

    def get_status(self) -> dict:
        with self._lock:
            return copy.deepcopy(self._state)

    def get_last_context_snapshot(self) -> dict:
        with self._lock:
            return {
                "scenario": self._state["scenario"],
                "raw_stdout": self._state["raw_stdout"],
                "parsed_metrics": copy.deepcopy(self._state["parsed_metrics"]),
                "last_philosophers": copy.deepcopy(self._state["last_philosophers"]),
            }

    def start_job(
        self, scenario: str, threads: int, items: int, buffer_size: int, no_sync: bool
    ) -> tuple:
        if scenario not in ALLOWED_SCENARIOS:
            return False, f"Неизвестный сценарий '{scenario}'."

        with self._lock:
            if self._state["running"]:
                return False, "Задача бенчмарка уже выполняется."

            self._state["job_id"] += 1
            job_id = self._state["job_id"]
            total_steps = len(THREAD_COUNTS) * 3 if scenario == "suite" else 1
            self._state.update(
                {
                    "running": True,
                    "scenario": scenario,
                    "step": 0,
                    "total_steps": total_steps,
                    "message": f"Запуск сценария {scenario}...",
                    "raw_stdout": "",
                    "parsed_metrics": [],
                    "error": None,
                    "started_at": time.time(),
                    "finished_at": None,
                }
            )
            if scenario == "philosophers":
                self._state["last_philosophers"] = None

        worker = threading.Thread(
            target=self._execute_job,
            args=(job_id, scenario, threads, items, buffer_size, no_sync),
            daemon=True,
        )
        worker.start()
        return True, job_id

    def _update(self, **kwargs):
        with self._lock:
            self._state.update(kwargs)

    def _execute_job(
        self,
        job_id: int,
        scenario: str,
        threads: int,
        items: int,
        buffer_size: int,
        no_sync: bool,
    ):
        try:
            if scenario == "suite":
                self._run_suite(items=items, buffer_size=buffer_size)
            elif scenario == "spinlock_test":
                bin_path = get_binary_path("spinlock_test")
                self._update(
                    message=f"Выполнение spinlock_test ({threads} потоков, {items} итераций)..."
                )
                rc, out, err = run_binary(
                    [str(bin_path), "--threads", str(threads), "--items", str(items)]
                )
                combined = out + ("\n" + err if err else "")
                if rc != 0:
                    raise RuntimeError(combined.strip() or f"Код возврата {rc}")
                parsed = parse_spinlock_output(out, threads)
                self._update(
                    step=1,
                    raw_stdout=combined,
                    parsed_metrics=parsed,
                    message=f"Завершено: spinlock_test ({threads} потоков).",
                )
            elif scenario == "prod_cons_sync":
                bin_path = get_binary_path("prod_cons_sync")
                self._update(
                    message=f"Выполнение prod_cons_sync ({threads}P/{threads}C, буфер={buffer_size})..."
                )
                rc, out, err = run_binary(
                    [
                        str(bin_path),
                        "--threads",
                        str(threads),
                        "--buffer",
                        str(buffer_size),
                        "--items",
                        str(items),
                    ]
                )
                combined = out + ("\n" + err if err else "")
                if rc != 0:
                    raise RuntimeError(combined.strip() or f"Код возврата {rc}")
                parsed = parse_prod_cons_output(out, threads)
                self._update(
                    step=1,
                    raw_stdout=combined,
                    parsed_metrics=parsed,
                    message=f"Завершено: prod_cons_sync ({threads} потоков).",
                )
            elif scenario == "aba_demo":
                bin_path = get_binary_path("aba_demo")
                self._update(
                    message=f"Выполнение aba_demo ({threads}P/{threads}C, {items} элементов)..."
                )
                rc, out, err = run_binary(
                    [str(bin_path), "--threads", str(threads), "--items", str(items)]
                )
                combined = out + ("\n" + err if err else "")
                if rc != 0:
                    raise RuntimeError(combined.strip() or f"Код возврата {rc}")
                parsed = parse_aba_demo_output(out, threads)
                self._update(
                    step=1,
                    raw_stdout=combined,
                    parsed_metrics=parsed,
                    message=f"Завершено: aba_demo ({threads} потоков).",
                )
            elif scenario == "philosophers":
                bin_path = get_binary_path("philosophers")
                meals = min(items, 5000)
                cmd = [
                    str(bin_path),
                    "--threads",
                    str(threads),
                    "--items",
                    str(meals),
                ]
                if no_sync:
                    cmd.append("--no-sync")
                mode_label = "наивный --no-sync" if no_sync else "иерархия Дейкстры"
                self._update(
                    message=f"Выполнение philosophers ({threads} потоков, {mode_label})..."
                )
                rc, out, err = run_binary(cmd)
                combined = out + ("\n" + err if err else "")
                if rc != 0:
                    raise RuntimeError(combined.strip() or f"Код возврата {rc}")
                philo_res = parse_philosophers_output(out, threads)
                msg = (
                    "Обнаружен Deadlock в наивном режиме!"
                    if philo_res["deadlock_detected"]
                    else f"Завершено: philosophers ({threads} потоков, без дедлоков)."
                )
                self._update(
                    step=1,
                    raw_stdout=combined,
                    parsed_metrics=[philo_res],
                    last_philosophers=philo_res,
                    message=msg,
                )
        except Exception as exc:
            self._update(
                error=str(exc),
                message=f"Ошибка выполнения: {exc}",
            )
        finally:
            self._update(running=False, finished_at=time.time())

    def _run_suite(self, items: int, buffer_size: int):
        spin_bin = get_binary_path("spinlock_test")
        pc_bin = get_binary_path("prod_cons_sync")
        aba_bin = get_binary_path("aba_demo")
        queue_items = max(1000, items // 2)

        all_records = []
        stdout_chunks = []
        step = 0

        for t in THREAD_COUNTS:
            step += 1
            self._update(step=step - 1, message=f"[{step}/15] spinlock_test при {t} потоках...")
            rc, out, err = run_binary(
                [str(spin_bin), "--threads", str(t), "--items", str(items)]
            )
            if rc != 0:
                raise RuntimeError(f"spinlock_test ({t}T) завершился с ошибкой: {err or out}")
            all_records.extend(parse_spinlock_output(out, t))
            stdout_chunks.append(f"=== spinlock_test ({t}T) ===\n{out.strip()}")

            step += 1
            self._update(step=step - 1, message=f"[{step}/15] prod_cons_sync при {t} потоках...")
            rc, out, err = run_binary(
                [
                    str(pc_bin),
                    "--threads",
                    str(t),
                    "--buffer",
                    str(buffer_size),
                    "--items",
                    str(queue_items),
                ]
            )
            if rc != 0:
                raise RuntimeError(f"prod_cons_sync ({t}T) завершился с ошибкой: {err or out}")
            all_records.extend(parse_prod_cons_output(out, t))
            stdout_chunks.append(f"=== prod_cons_sync ({t}T) ===\n{out.strip()}")

            step += 1
            self._update(step=step - 1, message=f"[{step}/15] aba_demo (MSQueue) при {t} потоках...")
            rc, out, err = run_binary(
                [str(aba_bin), "--threads", str(t), "--items", str(queue_items)]
            )
            if rc != 0:
                raise RuntimeError(f"aba_demo ({t}T) завершился с ошибкой: {err or out}")
            all_records.extend(parse_aba_demo_output(out, t))
            stdout_chunks.append(f"=== aba_demo ({t}T) ===\n{out.strip()}")

        save_csv_records(all_records)
        self._update(
            step=step,
            raw_stdout="\n\n".join(stdout_chunks),
            parsed_metrics=all_records,
            message=f"Полный пакет завершён ({len(all_records)} записей сохранено в data.csv).",
        )


def build_grounded_context(records: list, kpis: dict, snapshot: dict) -> str:
    """Construct compact benchmark context for Gemini or offline diagnostics."""
    lines = [
        f"Хост: {platform.system()} {platform.machine()}, логических CPU: {os.cpu_count() or 'неизвестно'}",
        f"Сводка KPI: {json.dumps(kpis, ensure_ascii=False)}",
        "Метрики CSV (потоки, тест, примитив, оп/с, задержка_нс, длительность_с):",
    ]
    for r in records:
        lines.append(
            f"- {r['threads']}T | {r['benchmark']} | {r['sync_primitive']} | "
            f"{r['throughput_ops_sec']:.2f} оп/с | {r['latency_ns']:.2f} нс | {r['duration_sec']:.4f} с"
        )
    if snapshot.get("scenario"):
        lines.append(f"Последний интерактивный сценарий: {snapshot['scenario']}")
    if snapshot.get("raw_stdout"):
        tail = snapshot["raw_stdout"][-2000:]
        lines.append(f"Фрагмент последнего вывода stdout:\n{tail}")
    return "\n".join(lines)


def offline_rule_based_diagnosis(
    question: str, records: list, kpis: dict, snapshot: dict
) -> str:
    """Deterministic Russian systems-engineering fallback when offline or API unavailable."""
    q = (question or "").lower()
    cpus = os.cpu_count() or "н/д"
    by_prim = group_series(records)
    spin_rows = by_prim.get("Spinlock", [])
    mutex_rows = by_prim.get("Pthread_Mutex", [])
    msq_rows = by_prim.get("LockFree_MSQueue", [])
    pc_rows = by_prim.get("Mutex+2Condvars", [])
    philo = snapshot.get("last_philosophers")

    parts = []

    if any(
        k in q
        for k in (
            "deadlock",
            "philosoph",
            "dijkstra",
            "coffman",
            "fork",
            "дедлок",
            "взаимн",
            "философ",
            "дейкстр",
            "коффман",
            "вилк",
        )
    ):
        if philo and philo.get("deadlock_detected"):
            parts.append(
                f"**Анализ Deadlock у обедающих философов ({philo['threads']} потоков):**\n"
                "- **Зафиксированное событие:** `[DEADLOCK DETECTED]` в наивном режиме `--no-sync`. "
                f"Выполнено приёмов пищи: `{philo.get('completed_meals', 0)}` до срабатывания watchdog-таймера (500 мс).\n"
                "- **Первопричина (условия Коффмана):** Каждый поток захватывал сначала левую вилку, а затем ожидал правую, "
                "формируя замкнутый цикл кругового ожидания (Circular Wait: P0 → F1 → P1 → ... → P0).\n"
                "- **Решение:** Отключите `--no-sync` для активации иерархии ресурсов Дейкстры (`min(left, right)` перед `max(left, right)`), что превращает граф распределения ресурсов в направленный ациклический граф (DAG)."
            )
        else:
            parts.append(
                "**Обедающие философы и предотвращение Deadlock:**\n"
                "- **Наивный режим (`--no-sync`):** Симметричный захват `left -> right` одновременно выполняет все 4 условия Коффмана "
                "(взаимное исключение, удержание и ожидание, отсутствие вытеснения, круговое ожидание), провоцируя Deadlock.\n"
                "- **Иерархия ресурсов Дейкстры (по умолчанию):** Захват сначала вилки с индексом `min(left, right)`, затем `max(left, right)` задаёт строгий глобальный порядок ресурсов и математически исключает круговое ожидание."
            )
        return "\n\n".join(parts)

    if any(
        k in q
        for k in (
            "msqueue",
            "lock-free",
            "lockfree",
            "aba",
            "queue",
            "condvar",
            "tagged",
            "очеред",
            "майкл",
            "скотт",
            "тегирован",
            "указател",
        )
    ):
        if msq_rows and pc_rows:
            speedup = kpis.get("msqueue_speedup")
            msq_hi = msq_rows[-1]
            pc_hi = pc_rows[-1]
            ratio_txt = (
                f"в **{speedup['ratio']}x** выше при {speedup['threads']} парах потоков"
                if speedup
                else "существенно выше"
            )
            parts.append(
                "**Lock-Free очередь Майкла — Скотта против блокирующего монитора (`Mutex + 2 Condvars`):**\n"
                f"- **Масштабируемость:** При {msq_hi['threads']}P/{msq_hi['threads']}C `LockFree_MSQueue` достигает "
                f"**{msq_hi['throughput_ops_sec']/1e6:.2f} млн оп/с** против `Mutex+2Condvars` с "
                f"**{pc_hi['throughput_ops_sec']/1e3:.1f} тыс. оп/с** (пропускная способность {ratio_txt}).\n"
                "- **Архитектурное отличие:** Единый `pthread_mutex_t` сериализует все операции `push` и `pop` на одном узле, а `MSQueue` разносит конкуренцию CAS между указателями `head_` и `tail_` и использует lock-free механизм *Helping*.\n"
                "- **Про задержку и ABA:** Повышенная средняя задержка в тесте `MSQueue` отражает время нахождения элемента в очереди при всплесках нагрузки, а `aba_demo` демонстрирует защиту от феномена `A -> B -> A` с помощью 64-битных **Tagged Pointers** (адрес + счётчик поколений)."
            )
        else:
            parts.append(
                "**Lock-Free очередь Майкла — Скотта (`MSQueue`):**\n"
                "- В `data.csv` пока нет сравнительных записей для `LockFree_MSQueue` и `Mutex+2Condvars`. Запустите полный пакет масштабирования для точных цифр.\n"
                "- **Архитектурный принцип:** `MSQueue` устраняет общую блокировку за счёт атомарных операций CAS на `head_` и `tail_` с защитой от проблемы ABA через 64-битные **Tagged Pointers**."
            )
        return "\n\n".join(parts)

    if any(
        k in q
        for k in (
            "spinlock",
            "mutex",
            "futex",
            "bouncing",
            "mesi",
            "cache",
            "спинлок",
            "мьютекс",
            "фьютекс",
            "кэш",
            "деградир",
            "деградац",
        )
    ):
        if spin_rows and mutex_rows:
            s_low, s_high = spin_rows[0], spin_rows[-1]
            m_low, m_high = mutex_rows[0], mutex_rows[-1]
            drop = (
                s_low["throughput_ops_sec"] / s_high["throughput_ops_sec"]
                if s_high["throughput_ops_sec"] > 0
                else 0.0
            )
            lat_ratio = (
                s_high["latency_ns"] / m_high["latency_ns"]
                if m_high["latency_ns"] > 0
                else 0.0
            )
            parts.append(
                f"**Диагностика конкуренции Spinlock против Pthread Mutex (логических CPU: {cpus}):**\n"
                f"- **Низкая конкуренция ({s_low['threads']} поток):** Spinlock показывает **{s_low['throughput_ops_sec']/1e6:.2f} млн оп/с** "
                f"(`{s_low['latency_ns']:.1f} нс`) против Pthread Mutex **{m_low['throughput_ops_sec']/1e6:.2f} млн оп/с** "
                f"(`{m_low['latency_ns']:.1f} нс`), так как полностью избегает системных вызовов ядра.\n"
                f"- **Высокая конкуренция ({s_high['threads']} потоков):** Пропускная способность Spinlock падает в **{drop:.1f}x** — до "
                f"**{s_high['throughput_ops_sec']/1e6:.2f} млн оп/с** (`{s_high['latency_ns']:.1f} нс`), тогда как Pthread Mutex удерживает "
                f"**{m_high['throughput_ops_sec']/1e6:.2f} млн оп/с** (`{m_high['latency_ns']:.1f} нс` — задержка в **{lat_ratio:.1f}x** ниже).\n"
                "- **Системная причина:** Активное ожидание на `atomic_flag` вызывает шторм инвалидации кэш-линий между ядрами по протоколу MESI/MOESI (cache-line bouncing) и сжигает такты CPU, когда потоков больше физических ядер. "
                "`pthread_mutex_t` опирается на двухфазный Linux **Futex** (`sys_futex(FUTEX_WAIT)`), усыпляя конкурирующие потоки в ядре."
            )
        else:
            parts.append(
                f"**Сравнение Spinlock и Pthread Mutex (логических CPU: {cpus}):**\n"
                "- Данных в `data.csv` для этого сравнения пока нет. Запустите сценарий `spinlock_test` или полный пакет масштабирования.\n"
                "- При низкой конкуренции Spinlock быстрее за счёт работы в пользовательском пространстве, а при росте числа потоков `pthread_mutex_t` (Futex) выигрывает за счёт усыпления ожидающих потоков в ядре."
            )
        return "\n\n".join(parts)

    # General bottleneck & full dataset overview
    winner = kpis.get("throughput_winner")
    low_lat = kpis.get("lowest_latency")
    alert = kpis.get("bottleneck_alert", {})
    speedup = kpis.get("msqueue_speedup")

    summary = [f"**Автономная диагностика узких мест (логических CPU: {cpus}):**"]
    if not records:
        summary.append(
            "- В `results/data.csv` пока отсутствуют замеры. Запустите **«Полный пакет масштабирования»** в панели слева."
        )
    if winner:
        summary.append(
            f"- **Пиковая пропускная способность:** `{winner['primitive']}` достиг **{winner['throughput_ops_sec']/1e6:.2f} млн оп/с** "
            f"при `{winner['threads']}` потоке(ах) (`{winner['benchmark']}`)."
        )
    if low_lat:
        summary.append(
            f"- **Минимальная задержка:** `{low_lat['primitive']}` показал **{low_lat['latency_ns']:.1f} нс** "
            f"при `{low_lat['threads']}` потоке(ах)."
        )
    if speedup:
        summary.append(
            f"- **Выигрыш Lock-Free очереди:** `LockFree_MSQueue` опередил `Mutex+2Condvars` в **{speedup['ratio']}x** "
            f"при `{speedup['threads']}` парах потоков-производителей и потребителей."
        )
    if alert.get("title"):
        summary.append(f"- **Главное узкое место (`{alert['title']}`):** {alert['detail']}")

    if spin_rows and len(spin_rows) >= 2:
        summary.append(
            "- **Инженерный вывод:** Применяйте `spinlock_t` только для сверхкоротких критических секций без переподписки ядер; "
            "при росте конкуренции переходите на `pthread_mutex_t` (Futex) или неблокирующую очередь `MSQueue`."
        )
    return "\n".join(summary)


def call_gemini_api(
    api_key: str, question: str, history: list, context_text: str
) -> tuple:
    """Call Gemini 2.5 Flash REST API via urllib.request in Russian. Never leaks api_key."""
    system_instruction = (
        "Ты — ведущий системный инженер Linux и специалист по параллельным вычислениям. "
        "Ты анализируешь живые бенчмарки примитивов синхронизации на C11/C++17 "
        "(POSIX mutex/futex, user-space spinlocks, условные переменные Месы, задача обедающих философов, "
        "Lock-Free очередь Майкла — Скотта и защита от ABA через Tagged Pointer). "
        "Отвечай всегда на чётком, лаконичном техническом русском языке, подходящем для живой презентации. "
        "Обязательно цитируй точные числа из переданных метрик CSV и вывода stdout. "
        "Общепринятые термины (Spinlock, Mutex, Futex, Lock-Free MSQueue, Tagged Pointer, MESI, Deadlock) оставляй в понятной технической форме."
    )

    contents = [
        {
            "role": "user",
            "parts": [
                {
                    "text": (
                        f"АКТУАЛЬНЫЙ КОНТЕКСТ БЕНЧМАРКОВ:\n{context_text}\n\n"
                        "Прими контекст к сведению и отвечай на вопросы пользователя на русском языке с опорой на точные цифры."
                    )
                }
            ],
        },
        {
            "role": "model",
            "parts": [
                {"text": "Принято. Буду анализировать результаты на русском языке, опираясь на точные наблюдаемые метрики."}
            ],
        },
    ]

    if isinstance(history, list):
        for turn in history[-6:]:
            if not isinstance(turn, dict):
                continue
            role = "model" if turn.get("role") == "assistant" else "user"
            text = str(turn.get("text", "")).strip()
            if text:
                contents.append({"role": role, "parts": [{"text": text[:1500]}]})

    contents.append({"role": "user", "parts": [{"text": question.strip()[:2000]}]})

    payload = {
        "system_instruction": {"parts": [{"text": system_instruction}]},
        "contents": contents,
        "generationConfig": {"temperature": 0.25, "maxOutputTokens": 700},
    }

    req = urllib.request.Request(
        GEMINI_ENDPOINT,
        data=json.dumps(payload).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "x-goog-api-key": api_key,
        },
        method="POST",
    )

    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            body = json.loads(resp.read().decode("utf-8"))
        candidates = body.get("candidates") or []
        if not candidates:
            return None, "Пустой ответ от Gemini API"
        parts = candidates[0].get("content", {}).get("parts", [])
        text = "".join(p.get("text", "") for p in parts).strip()
        if not text:
            return None, "Пустая генерация Gemini API"
        return text, None
    except urllib.error.HTTPError as exc:
        return None, f"Ошибка Gemini HTTP {exc.code}"
    except urllib.error.URLError:
        return None, "Нет подключения к сети"
    except TimeoutError:
        return None, "Таймаут запроса к Gemini"
    except Exception:
        return None, "Сбой запроса к Gemini"


BENCHMARK_MANAGER = BenchmarkManager()


class DashboardHandler(BaseHTTPRequestHandler):
    """HTTP request handler serving static UI assets and JSON API routes."""

    server_version = "Lab4SyncDashboard/1.0"

    def log_message(self, fmt, *args):
        # Clean concise request logs; never prints headers or secret material
        sys.stderr.write(f"[http] {self.address_string()} - {fmt % args}\n")

    def _send_json(self, status: int, payload: dict):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _read_json_body(self) -> dict:
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = 0
        if length <= 0 or length > 262144:
            return {}
        raw = self.rfile.read(length).decode("utf-8", errors="replace")
        try:
            parsed = json.loads(raw)
            return parsed if isinstance(parsed, dict) else {}
        except json.JSONDecodeError:
            return {}

    def _serve_static(self, url_path: str):
        rel = "index.html" if url_path in ("/", "") else url_path.lstrip("/")
        target = (DASHBOARD_DIR / rel).resolve()
        try:
            target.relative_to(DASHBOARD_DIR)
        except ValueError:
            self.send_error(403, "Forbidden")
            return

        if not target.is_file():
            self.send_error(404, "Not Found")
            return

        ctype, _ = mimetypes.guess_type(str(target))
        if target.suffix == ".js":
            ctype = "application/javascript; charset=utf-8"
        elif target.suffix == ".html":
            ctype = "text/html; charset=utf-8"
        elif not ctype:
            ctype = "application/octet-stream"

        data = target.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        if target.suffix == ".html":
            self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path

        if path == "/api/health":
            key_configured = bool(_load_gemini_api_key())
            self._send_json(
                200,
                {
                    "status": "ok",
                    "cpu_count": os.cpu_count() or 1,
                    "os": platform.system(),
                    "arch": platform.machine(),
                    "binaries": get_binary_availability(),
                    "ai_configured": key_configured,
                    "ai_mode": "gemini" if key_configured else "offline",
                },
            )
            return

        if path == "/api/results":
            records = load_csv_records()
            snapshot = BENCHMARK_MANAGER.get_last_context_snapshot()
            kpis = compute_kpis(records, snapshot.get("last_philosophers"))
            self._send_json(
                200,
                {
                    "records": records,
                    "series": group_series(records),
                    "kpis": kpis,
                },
            )
            return

        if path == "/api/run/status":
            status = BENCHMARK_MANAGER.get_status()
            if not status["running"]:
                records = load_csv_records()
                status["updated_results"] = {
                    "records": records,
                    "series": group_series(records),
                    "kpis": compute_kpis(records, status.get("last_philosophers")),
                }
            self._send_json(200, status)
            return

        self._serve_static(path)

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path

        if path == "/api/run":
            body = self._read_json_body()
            scenario = str(body.get("scenario", "spinlock_test")).strip()
            threads = clamp_int(body.get("threads"), 4, 1, 32)
            items = clamp_int(body.get("items"), 100000, 100, 2000000)
            buffer_size = clamp_int(body.get("buffer"), 16, 2, 1024)
            no_sync = bool(body.get("no_sync", False))

            ok, info = BENCHMARK_MANAGER.start_job(
                scenario=scenario,
                threads=threads,
                items=items,
                buffer_size=buffer_size,
                no_sync=no_sync,
            )
            if not ok:
                code = 409 if "уже выполняется" in str(info) else 400
                self._send_json(code, {"error": str(info)})
                return
            self._send_json(202, {"started": True, "job_id": info})
            return

        if path == "/api/ai/chat":
            body = self._read_json_body()
            question = str(body.get("question", "")).strip()
            if not question:
                self._send_json(400, {"error": "Вопрос не может быть пустым."})
                return
            history = body.get("history", [])

            records = load_csv_records()
            snapshot = BENCHMARK_MANAGER.get_last_context_snapshot()
            kpis = compute_kpis(records, snapshot.get("last_philosophers"))
            context_text = build_grounded_context(records, kpis, snapshot)

            api_key = _load_gemini_api_key()
            if api_key:
                reply, err_reason = call_gemini_api(
                    api_key=api_key,
                    question=question,
                    history=history,
                    context_text=context_text,
                )
                if reply:
                    self._send_json(
                        200,
                        {
                            "reply": reply,
                            "mode": "gemini",
                            "reason": "Подключено к Gemini 2.5 Flash",
                        },
                    )
                    return
                fallback = offline_rule_based_diagnosis(
                    question, records, kpis, snapshot
                )
                self._send_json(
                    200,
                    {
                        "reply": fallback,
                        "mode": "offline",
                        "reason": f"Автоматический резерв ({err_reason})",
                    },
                )
                return

            fallback = offline_rule_based_diagnosis(
                question, records, kpis, snapshot
            )
            self._send_json(
                200,
                {
                    "reply": fallback,
                    "mode": "offline",
                    "reason": "Автономный анализ (API-ключ не задан)",
                },
            )
            return

        self.send_error(404, "Not Found")


def find_free_port(host: str, preferred: int) -> int:
    for port in range(preferred, preferred + 20):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                sock.bind((host, port))
                return port
            except OSError:
                continue
    return preferred


def main():
    parser = argparse.ArgumentParser(
        description="Zero-dependency local dashboard server for lab4_sync."
    )
    parser.add_argument("--host", default="127.0.0.1", help="Bind host (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8080, help="Bind port (default: 8080)")
    args = parser.parse_args()

    port = args.port
    server = ThreadingHTTPServer((args.host, port), DashboardHandler)
    ai_status = "Gemini Онлайн" if _load_gemini_api_key() else "Автономный режим"
    print(f"[lab4_sync] Веб-панель запущена на http://{args.host}:{port} (ИИ: {ai_status})")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[lab4_sync] Остановка сервера панели.")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
