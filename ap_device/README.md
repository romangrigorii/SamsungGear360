# Gear 360 AP relay device (Raspberry Pi)

Run this on a **Raspberry Pi** so it connects to the Gear 360 camera’s WiFi AP, pulls the live MJPEG stream, and re-serves it on your LAN. Your PC can then open the stream at **`http://<PI_IP>:7679/livestream_high.avi`** without being on the camera’s WiFi.

## Folder layout

```
ap_device/
├── config/
│   ├── camera_wifi.conf           # SSID/password for camera WiFi (gitignored, create from .example)
│   ├── camera_wifi.conf.example   # Template — copy to camera_wifi.conf and fill in
│   ├── pc_defaults.ps1            # PC-side defaults (Pi host/user) — create from .example
│   ├── pc_defaults.ps1.example    # Template — copy to pc_defaults.ps1 and fill in
│   └── relay.conf                 # Relay options reference (not loaded; args go to relay_stream.py)
├── connect_camera_wifi.sh         # On Pi: connect WiFi to camera AP
├── list_wifi.sh                   # On Pi: list visible WiFi networks (debug SSID mismatches)
├── relay_stream.py                # On Pi: stream relay (camera → LAN)
├── start_relay.sh                 # On Pi: launch relay in background (called by run_from_pc.ps1)
├── run_from_pc.ps1                # On PC: orchestrator — sync, connect WiFi, start relay, report
└── README.md
```

## Run everything from your PC (recommended)

One script on your PC syncs files to the Pi, connects the Pi to the camera WiFi, starts the relay, and prints the stream URL.

**First-time setup**

1. **Pi host/user:** Copy and edit PC-side defaults:
   ```powershell
   cd SamsungGear360\ap_device
   copy config\pc_defaults.example config\pc_defaults.ps1
   notepad config\pc_defaults.ps1   # set PI_HOST (e.g. 10.0.0.210), PI_USER (e.g. roman)
   ```

2. **Camera WiFi:** Create the camera WiFi config (on PC or Pi). On PC it will be synced to the Pi:
   ```powershell
   copy config\camera_wifi.conf.example config\camera_wifi.conf
   notepad config\camera_wifi.conf   # set SSID= and PASSWORD= from iOS Connect
   ```

3. **SSH:** You’ll be prompted for the Pi password **once**; the script reuses the same SSH connection for all steps.

**Run**

From PowerShell (from `SamsungGear360\ap_device` or from repo root):

```powershell
cd SamsungGear360\ap_device
.\run_from_pc.ps1
```

Options:

```powershell
.\run_from_pc.ps1 -PiHost 10.0.0.210 -PiUser roman
.\run_from_pc.ps1 -SkipSync          # don't copy files (use existing on Pi)
.\run_from_pc.ps1 -SkipWifi          # don't run connect_camera_wifi (already connected)
.\run_from_pc.ps1 -SkipRelay         # only sync and/or wifi, don't start relay
.\run_from_pc.ps1 -RelayPort 9680
.\run_from_pc.ps1 -ListWifi          # see which networks the Pi sees (to fix SSID mismatch)
.\run_from_pc.ps1 -LaunchViewer      # after setup, open the Gear 360 viewer on this PC
```

The script prints a short report and the **stream URL** to open on your PC.

**If the viewer says "Connection timed out" or "502 Bad Gateway"**

Check in order:

1. **Gear 360:** Camera is on, in **live view / streaming** (e.g. opened via Gear 360 app or iOS Connect so it starts the stream).
2. **Camera WiFi:** Pi is connected to the camera’s AP (SSID like `Gear 360(B8F1)`). Run with `-ListWifi` to see networks; run without `-SkipWifi` so the script connects the Pi to the camera.
3. **Relay:** Relay is running on the Pi. In the script report, "Start stream relay" should be OK. If it failed, SSH to the Pi and run `cd ~/ap_device && bash start_relay.sh 7679`, then check `~/ap_device/relay.log`.
4. **PC → Pi:** From the PC, the Pi’s IP (e.g. `10.0.0.210`) and port `7679` must be reachable (no firewall blocking).

After changing anything, run `.\run_from_pc.ps1` again (use `-SkipSync` or `-SkipWifi` if you only restarted the relay).

---

## How it works

- **Gear 360** creates its own WiFi AP (e.g. `192.168.43.1`) and serves the stream at `http://192.168.43.1:7679/livestream_high.avi`.
- **Pi** has two network connections:
  1. **WiFi** → connect to the Gear 360’s AP (so the Pi can reach `192.168.43.1`).
  2. **Ethernet** (or second WiFi) → your main network (e.g. `10.0.0.x`).
- **This relay** runs on the Pi: it pulls the stream from the camera and serves it on the Pi’s LAN IP and port. Your PC connects to the Pi’s IP and port.

```
  [Gear 360 camera]  ----- WiFi ----->  [Raspberry Pi]  ----- Ethernet/WiFi ----->  [Your PC]
  192.168.43.1:7679                      relay_stream.py                            http://PI_IP:7679/...
```

## Setup on the Pi (manual)

### 1. Network

- Connect the Pi’s **WiFi** to the Gear 360’s AP (same network as the camera, typically `192.168.43.x`).
- Connect the Pi’s **Ethernet** to your router so it gets an IP on your LAN (e.g. `10.0.0.xxx`).

**Connect to the camera’s WiFi:**

```bash
cd ~/ap_device
cp config/camera_wifi.conf.example config/camera_wifi.conf
nano config/camera_wifi.conf   # set SSID= and PASSWORD= (from iOS Connect)
chmod +x connect_camera_wifi.sh
./connect_camera_wifi.sh
```

Or pass SSID/password directly:

```bash
./connect_camera_wifi.sh "Gear360_XXXX" "your_camera_wifi_password"
./connect_camera_wifi.sh --disconnect   # return to normal WiFi
```

### 2. Run the relay

```bash
cd ~/ap_device
python3 relay_stream.py
# or in background (same as run_from_pc.ps1 does):
bash start_relay.sh
```

**No extra packages:** the script uses only Python 3 stdlib.

## Use from your PC

1. Start the relay (via `run_from_pc.ps1` or manually on the Pi).
2. On your PC, open the stream URL (printed by `run_from_pc.ps1`) or run:

   ```bash
   python python/gear360_viewer.py http://<PI_IP>:7679/livestream_high.avi
   ```

   Or with FFplay:

   ```bash
   ffplay -hide_banner -fflags nobuffer -flags low_delay -framedrop -i "http://<PI_IP>:7679/livestream_high.avi"
   ```

Replace `<PI_IP>` with the Pi’s IP on your LAN.

## Troubleshooting

- **"No network with SSID '...'"** — The Pi can’t see the camera’s WiFi. Check: (1) Gear 360 is **on** and in **live view / streaming**; (2) SSID and PASSWORD in `config/camera_wifi.conf` match the camera (no extra spaces/CRLF); (3) Pi WiFi is on and in range. Run **`.\run_from_pc.ps1 -ListWifi`** to see exactly which networks the Pi sees and compare the SSID (e.g. "Gear 360(B8F1)" vs "Gear360(B8F1)").
- **"Not authorized to control networking"** — The connect step runs `sudo nmcli` on the Pi. Either (1) run the connect script once over SSH and enter your Pi user’s sudo password when prompted, or (2) allow passwordless sudo for nmcli: `sudo visudo` and add a line like `roman ALL=(ALL) NOPASSWD: /usr/bin/nmcli`.
- **Password asked more than once** — To avoid SSH password prompts, use an SSH key: `ssh-copy-id roman@10.0.0.210`.

## Relay options

| Option        | Default                          | Description |
|---------------|-----------------------------------|-------------|
| `--source-url`| `http://192.168.43.1:7679/livestream_high.avi` | Camera stream URL. |
| `--bind`      | `0.0.0.0`                         | Bind address. |
| `--port`      | `7679`                            | Port the relay listens on. |
| `--allow-ip`  | (empty = allow all)               | Comma-separated client IPs allowed. |

## Run relay at boot (optional)

On the Pi:

```bash
sudo nano /etc/systemd/system/gear360-relay.service
```

Paste (adjust paths/username if needed):

```ini
[Unit]
Description=Gear 360 stream relay
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=pi
WorkingDirectory=/home/pi/ap_device
ExecStart=/usr/bin/python3 /home/pi/ap_device/relay_stream.py --bind 0.0.0.0 --port 7679
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Then: `sudo systemctl daemon-reload && sudo systemctl enable gear360-relay && sudo systemctl start gear360-relay`
