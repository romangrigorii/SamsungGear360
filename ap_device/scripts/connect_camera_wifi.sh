#!/bin/bash
# Connect the Raspberry Pi's WiFi to the Gear 360 camera's AP.
# Use the SSID and password you got from the camera (e.g. via iOS Connect).
#
# Usage:
#   ./connect_camera_wifi.sh                          # read SSID/password from camera_wifi.conf
#   ./connect_camera_wifi.sh "MyGear360" "mypass"     # SSID and password as arguments
#   ./connect_camera_wifi.sh --ssid "MyGear360" --password "mypass"
#
# Notes:
# - This script is intended to run on the Pi.
# - By default it uses interface wlan0, and config at ../config/camera_wifi.conf

set -e

SCRIPT_DIR="$(dirname "$0")"
AP_DEVICE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Prefer config/camera_wifi.conf, then legacy camera_wifi.conf in ap_device dir
CONF_FILE="$AP_DEVICE_DIR/config/camera_wifi.conf"
[ -f "$CONF_FILE" ] || CONF_FILE="$AP_DEVICE_DIR/camera_wifi.conf"
INTERFACE="wlan0"

usage() {
    echo "Usage: $0 [OPTIONS] [SSID PASSWORD]"
    echo ""
    echo "Connect this device's WiFi to the Gear 360 camera AP."
    echo ""
    echo "Options:"
    echo "  --ssid SSID         Camera WiFi name (SSID)"
    echo "  --password PASS     Camera WiFi password"
    echo "  --interface IFACE   WiFi interface (default: wlan0)"
    echo "  --disconnect        Disconnect from camera WiFi (re-enable normal WiFi behavior)"
    echo "  -h, --help          Show this help"
    echo ""
    echo "If neither --ssid/--password nor SSID PASSWORD are given, reads from: $CONF_FILE"
    echo "  Create from example: cp config/camera_wifi.conf.example config/camera_wifi.conf && nano config/camera_wifi.conf"
    exit 0
}

# Parse args
SSID=""
PASSWORD=""
DISCONNECT=false

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage ;;
        --ssid)    SSID="$2"; shift 2 ;;
        --password) PASSWORD="$2"; shift 2 ;;
        --interface) INTERFACE="$2"; shift 2 ;;
        --disconnect) DISCONNECT=true; shift ;;
        *)
            if [ -z "$SSID" ]; then
                SSID="$1"
            elif [ -z "$PASSWORD" ]; then
                PASSWORD="$1"
            fi
            shift
            ;;
    esac
done

# Read from config file if SSID/password not set
if [ -z "$SSID" ] && [ -f "$CONF_FILE" ]; then
    echo "Reading from $CONF_FILE"
    while IFS= read -r line; do
        [[ "$line" =~ ^#.*$ ]] && continue
        if [[ "$line" =~ ^SSID=(.+)$ ]]; then
            SSID="${BASH_REMATCH[1]}"
            SSID="${SSID%\"}"; SSID="${SSID#\"}"
            SSID="${SSID%\'}"; SSID="${SSID#\'}"
            SSID="${SSID//$'\r'/}"; SSID="${SSID//$'\n'/}"
            SSID="${SSID# }"; SSID="${SSID% }"
        elif [[ "$line" =~ ^PASSWORD=(.+)$ ]]; then
            PASSWORD="${BASH_REMATCH[1]}"
            PASSWORD="${PASSWORD%\"}"; PASSWORD="${PASSWORD#\"}"
            PASSWORD="${PASSWORD%\'}"; PASSWORD="${PASSWORD#\'}"
            PASSWORD="${PASSWORD//$'\r'/}"; PASSWORD="${PASSWORD//$'\n'/}"
            PASSWORD="${PASSWORD# }"; PASSWORD="${PASSWORD% }"
        elif [[ "$line" =~ ^INTERFACE=(.+)$ ]]; then
            INTERFACE="${BASH_REMATCH[1]}"
            INTERFACE="${INTERFACE%\"}"; INTERFACE="${INTERFACE#\"}"
            INTERFACE="${INTERFACE//$'\r'/}"
            INTERFACE="${INTERFACE# }"; INTERFACE="${INTERFACE% }"
        fi
    done < "$CONF_FILE"
fi

if [ "$DISCONNECT" = true ]; then
    echo "Disconnecting from camera WiFi and re-enabling default WiFi..."
    if command -v nmcli &>/dev/null; then
        sudo nmcli connection down "$(nmcli -t -f NAME,DEVICE connection show --active | grep "$INTERFACE" | cut -d: -f1)" 2>/dev/null || true
        sudo nmcli device set "$INTERFACE" autoconnect true
        echo "Done. Interface $INTERFACE will use saved connections."
    else
        echo "Restart wpa_supplicant or reboot to return to normal WiFi. (e.g. sudo systemctl restart wpa_supplicant)"
    fi
    exit 0
fi

if [ -z "$SSID" ] || [ -z "$PASSWORD" ]; then
    echo "Error: Need camera WiFi SSID and password." >&2
    echo "  Either create $CONF_FILE (see camera_wifi.conf.example) or run:" >&2
    echo "  $0 --ssid \"CameraSSID\" --password \"CameraPassword\"" >&2
    exit 1
fi

echo "Connecting to camera WiFi: $SSID (interface: $INTERFACE)"

# Skip if already connected to the camera (same SSID or already on camera subnet 192.168.43.x)
if command -v nmcli &>/dev/null; then
    current_conn=$(nmcli -t -f NAME,DEVICE connection show --active 2>/dev/null | grep ":${INTERFACE}$" | head -1 | cut -d: -f1)
    if [ -n "$current_conn" ]; then
        current_ssid=$(nmcli -t -f 802-11-wireless.ssid connection show "$current_conn" 2>/dev/null | head -1)
        if [ "$current_ssid" = "$SSID" ]; then
            echo "Already connected to camera WiFi ($SSID). Skipping."
            echo "Camera stream should be at: http://192.168.43.1:7679/livestream_high.avi"
            exit 0
        fi
        # Same network with different spelling (e.g. extra space)
        if echo "$current_ssid" | grep -qi "Gear.*360"; then
            echo "Already connected to camera WiFi (SSID like '$current_ssid'). Skipping."
            echo "Camera stream should be at: http://192.168.43.1:7679/livestream_high.avi"
            exit 0
        fi
    fi
fi
if ip -4 addr show "$INTERFACE" 2>/dev/null | grep -q "192.168.43\."; then
    echo "Already on camera network (192.168.43.x). Skipping reconnect."
    echo "Camera stream should be at: http://192.168.43.1:7679/livestream_high.avi"
    exit 0
fi

if command -v nmcli &>/dev/null; then
    # Rescan so we see the camera AP; then try exact SSID or fuzzy match (e.g. "Gear 360 (B8F1)" vs "Gear 360(B8F1)")
    sudo nmcli device wifi rescan 2>/dev/null || true
    sleep 2
    if ! sudo nmcli device wifi connect "$SSID" password "$PASSWORD" ifname "$INTERFACE" 2>/dev/null; then
        # Exact SSID not found - try any visible network whose SSID contains "Gear" and "360"
        found_ssid=""
        while IFS= read -r line; do
            # nmcli -t -f SSID,SIGNAL device wifi list -> SSID:SIGNAL
            scan_ssid="${line%%:*}"
            [ -z "$scan_ssid" ] && continue
            if echo "$scan_ssid" | grep -qi "Gear" && echo "$scan_ssid" | grep -q "360"; then
                found_ssid="$scan_ssid"
                break
            fi
        done < <(nmcli -t -f SSID,SIGNAL device wifi list 2>/dev/null)
        if [ -n "$found_ssid" ]; then
            echo "Exact SSID '$SSID' not in scan; trying similar: '$found_ssid' (update config to this SSID to avoid this)"
            sudo nmcli device wifi connect "$found_ssid" password "$PASSWORD" ifname "$INTERFACE"
        else
            echo "Error: No network with SSID '$SSID' found, and no 'Gear 360' network in scan." >&2
            echo "  Run from PC: .\\run_from_pc.ps1 -ListWifi  to see exact SSID, then update config/camera_wifi.conf" >&2
            echo "  If the stream already works, use: .\\run_from_pc.ps1 -SkipWifi" >&2
            exit 1
        fi
    fi
    echo "Connected. Check with: ip addr show $INTERFACE"
    echo "Camera stream should be at: http://192.168.43.1:7679/livestream_high.avi"
elif [ -f /etc/wpa_supplicant/wpa_supplicant.conf ] || [ -f /etc/wpa_supplicant/wpa_supplicant-wlan0.conf ]; then
    # wpa_supplicant: add network and trigger reconnect
    WPA_CONF="/etc/wpa_supplicant/wpa_supplicant.conf"
    [ -f /etc/wpa_supplicant/wpa_supplicant-"$INTERFACE".conf ] && WPA_CONF="/etc/wpa_supplicant/wpa_supplicant-$INTERFACE.conf"
    # Append new network (requires sudo for /etc/wpa_supplicant/)
    sudo tee -a "$WPA_CONF" >/dev/null <<EOF

# Gear 360 camera (added by connect_camera_wifi.sh)
network={
    ssid="$SSID"
    psk="$PASSWORD"
    priority=100
}
EOF
    echo "Added network to $WPA_CONF. Reconnecting..."
    sudo wpa_cli -i "$INTERFACE" reconfigure 2>/dev/null || sudo systemctl restart wpa_supplicant
    echo "Reconnect requested. After a few seconds, check: ip addr show $INTERFACE"
    echo "Camera stream should be at: http://192.168.43.1:7679/livestream_high.avi"
else
    echo "No nmcli or wpa_supplicant config found. Install NetworkManager or configure WiFi manually." >&2
    echo "  With NetworkManager: nmcli device wifi connect \"$SSID\" password \"$PASSWORD\"" >&2
    exit 1
fi

