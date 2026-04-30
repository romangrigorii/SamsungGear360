#!/bin/bash
# Start the Gear 360 stream relay (called from run_from_pc.ps1).
# Usage: ./start_relay.sh [port]
# Default port: 7679
#
# Optional: ~/ap_device/config/relay.conf may contain:
#   SOURCE_URL=...
#   BIND=...          # bind address (often 0.0.0.0 for all interfaces)
#   PORT=...          # overrides [port] argument if set
#   ALLOW_IP=...      # comma-separated allow-list for clients

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

SOURCE_URL=""
BIND=""
ALLOW_IP=""
CONF_PORT=""

CONF="$AP_DEVICE/config/relay.conf"
if [ -f "$CONF" ]; then
    while IFS= read -r raw || [ -n "$raw" ]; do
        line="$(echo "$raw" | tr -d '\r')"
        [[ -z "$line" || "$line" =~ ^[[:space:]]*# ]] && continue
        [[ "$line" != *"="* ]] && continue
        key="${line%%=*}"
        val="${line#*=}"
        key="$(echo "$key" | tr -d '[:space:]')"
        val="$(echo "$val" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' -e 's/^"\(.*\)"$/\1/' -e "s/^'\(.*\)'$/\1/")"
        case "$key" in
            SOURCE_URL) SOURCE_URL="$val" ;;
            BIND)       BIND="$val" ;;
            ALLOW_IP)   ALLOW_IP="$val" ;;
            PORT)       CONF_PORT="$val" ;;
        esac
    done < "$CONF"
fi

if [ -n "$CONF_PORT" ]; then
    PORT="$CONF_PORT"
fi

RELAY_CMD=(python3 scripts/relay_stream.py --port "$PORT")
if [ -n "$SOURCE_URL" ]; then
    RELAY_CMD+=(--source-url "$SOURCE_URL")
fi
if [ -n "$BIND" ]; then
    RELAY_CMD+=(--bind "$BIND")
fi
if [ -n "$ALLOW_IP" ]; then
    RELAY_CMD+=(--allow-ip "$ALLOW_IP")
fi

nohup "${RELAY_CMD[@]}" >> "$LOG" 2>&1 &
sleep 2
if pgrep -f relay_stream.py >/dev/null; then
    echo started
else
    echo failed
    cat "$LOG" 1>&2
fi
