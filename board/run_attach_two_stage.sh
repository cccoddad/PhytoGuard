#!/bin/sh
# Two-stage inference launcher for SS928V100 board.
# Uses the new two-stage pipeline: plant classifier → disease classifier.
set -eu

BASE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LOG_PATH=/tmp/heshi_v2_vio.log
VIO_PID=""
SLEEP_PID=""
L610_PID=""

cleanup() {
    if [ -n "$L610_PID" ]; then
        kill "$L610_PID" 2>/dev/null || true
    fi
    if [ -n "$VIO_PID" ]; then
        kill "$VIO_PID" 2>/dev/null || true
    fi
    if [ -n "$SLEEP_PID" ]; then
        kill "$SLEEP_PID" 2>/dev/null || true
    fi
    wait 2>/dev/null || true
}

trap cleanup EXIT INT TERM

echo "[run_attach] starting board sample_vio (single sensor0)..."

# Start L610 4G connection in background
L610_SCRIPT="$BASE_DIR/l610_connect.sh"
if [ -x "$L610_SCRIPT" ]; then
    echo "[run_attach] starting L610 4G connection..."
    "$L610_SCRIPT" &
    L610_PID=$!
fi

# Start sample_vio to configure sensor + VI/VPSS
sleep 2147483647 | /opt/sample/mipi_rx/os08a20/sample_vio 0 0 \
    >"$LOG_PATH" 2>&1 &
VIO_PID=$!

# Wait for sensor init
echo "[run_attach] waiting for sensor init..."
for i in $(seq 1 30); do
    if grep -q "init success" "$LOG_PATH" 2>/dev/null; then
        echo "[run_attach] sensor init OK (waited ${i}s)"
        break
    fi
    if ! kill -0 "$VIO_PID" 2>/dev/null; then
        echo "[run_attach] ERROR: sample_vio died"
        cat "$LOG_PATH" 2>/dev/null | tail -20
        exit 1
    fi
    sleep 1
done

echo "[run_attach] starting two-stage infer..."
export LD_LIBRARY_PATH=/opt/lib:/opt/lib/npu:$LD_LIBRARY_PATH
"$BASE_DIR/heshi_v2_dual_infer" \
    --two-stage \
    --attach-only

echo "[run_attach] done."
