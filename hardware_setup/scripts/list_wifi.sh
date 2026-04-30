#!/bin/bash
# List WiFi networks visible from this device (run on Pi to compare with camera SSID).
# Usage: ./list_wifi.sh   or   bash list_wifi.sh

if command -v nmcli &>/dev/null; then
    echo "--- Networks visible (nmcli) ---"
    nmcli -t -f SSID,SIGNAL,SECURITY device wifi list 2>/dev/null | while IFS=: read -r ssid signal sec; do
        [ -n "$ssid" ] && echo "  SSID: [$ssid]  Signal: ${signal}%  Security: $sec"
    done
elif command -v iwlist &>/dev/null; then
    echo "--- Networks visible (iwlist) ---"
    iwlist wlan0 scan 2>/dev/null | grep -E "ESSID|Quality" | paste - - | sed 's/.*Quality=\([0-9]*\).*ESSID:"\(.*\)"/  SSID: [\2]  Signal: \1/'
else
    echo "No nmcli or iwlist found."
fi
echo ""
echo "Compare the SSID in brackets above with config/camera_wifi.conf (exact match required)."

