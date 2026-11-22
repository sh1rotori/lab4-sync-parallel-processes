#!/usr/bin/env bash
# ==============================================================================
# launch_dashboard.sh
# One-click offline launcher for the lab4_sync Visual Web Dashboard on Linux.
# Zero pip dependencies required.
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if command -v python3 >/dev/null 2>&1; then
    PY_BIN="python3"
elif command -v python >/dev/null 2>&1; then
    PY_BIN="python"
else
    echo "[ERROR] python3 is required to run the dashboard server." >&2
    exit 1
fi

# Check & compile required C/C++ binaries if missing
REQUIRED_BINS=("spinlock_test" "prod_cons_sync" "aba_demo" "philosophers")
MISSING=0
for b in "${REQUIRED_BINS[@]}"; do
    if [ ! -f "bin/$b" ] && [ ! -f "bin/$b.exe" ]; then
        MISSING=1
        break
    fi
done

if [ "$MISSING" -eq 1 ]; then
    echo "[lab4_sync] Compiled binaries missing. Running 'make all'..."
    if ! command -v make >/dev/null 2>&1; then
        echo "[ERROR] 'make' is not installed. Please install build-essential/make." >&2
        exit 1
    fi
    make all
fi

# Find an available local TCP port starting at 8080
PORT=$("$PY_BIN" -c '
import socket
for p in range(8080, 8100):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind(("127.0.0.1", p))
            print(p)
            break
        except OSError:
            continue
else:
    print(8080)
')

URL="http://127.0.0.1:$PORT"
SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        echo ""
        echo "[lab4_sync] Stopping dashboard server (PID $SERVER_PID)..."
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup INT TERM EXIT

echo "[lab4_sync] Starting dashboard server on $URL ..."
"$PY_BIN" scripts/dashboard_server.py --host 127.0.0.1 --port "$PORT" &
SERVER_PID=$!

# Poll /api/health until ready (max ~10s)
READY=0
for _ in $(seq 1 40); do
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "[ERROR] Dashboard server exited unexpectedly." >&2
        exit 1
    fi
    if "$PY_BIN" -c "import urllib.request; urllib.request.urlopen('$URL/api/health', timeout=1).read()" >/dev/null 2>&1; then
        READY=1
        break
    fi
    sleep 0.25
done

if [ "$READY" -ne 1 ]; then
    echo "[ERROR] Timed out waiting for dashboard server at $URL." >&2
    exit 1
fi

echo "[lab4_sync] Dashboard is live at: $URL"
echo "[lab4_sync] Press Ctrl+C in this terminal to stop."

if command -v xdg-open >/dev/null 2>&1; then
    xdg-open "$URL" >/dev/null 2>&1 &
elif command -v sensible-browser >/dev/null 2>&1; then
    sensible-browser "$URL" >/dev/null 2>&1 &
else
    echo "[INFO] Open $URL manually in your browser."
fi

wait "$SERVER_PID"
