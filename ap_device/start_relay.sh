#!/bin/bash
# Start the Gear 360 stream relay (called from run_from_pc.ps1).
# Usage: ./start_relay.sh [port]
# Default port: 7679

set -e
PORT="${1:-7679}"
AP_DEVICE="${HOME}/ap_device"
LOG="${AP_DEVICE}/relay.log"

cd "$AP_DEVICE"
# Kill any existing relay so the port is free and we run the latest code
pkill -9 -f relay_stream.py 2>/dev/null || true
sleep 2

echo "--- $(date) ---" > "$LOG"
which python3 >> "$LOG" 2>&1
python3 --version >> "$LOG" 2>&1
nohup python3 relay_stream.py --port "$PORT" >> "$LOG" 2>&1 &
sleep 2
if pgrep -f relay_stream.py >/dev/null; then
    echo started
else
    echo failed
    cat "$LOG" 1>&2
fi
