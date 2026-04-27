# Gear 360 → Raspberry Pi relay (fast setup)

Goal: view the Gear 360 live stream **from your normal network** (so your PC stays on Wi‑Fi) at:

- `http://<PI_LAN_IP>:7679/livestream_high.avi`

### What you need

- **Raspberry Pi** connected to your router via **Ethernet** (recommended).
- Pi **Wi‑Fi** connects to the Gear 360 camera’s Wi‑Fi AP.
- SSH enabled on the Pi (so your PC can run the setup script).

### 1) Set camera Wi‑Fi credentials (on your PC)

Edit:

- `config/camera_wifi.conf`

Set:

```text
SSID=Gear 360(B8F1)
PASSWORD=your_password
```

### 2) Set Pi IP/user (on your PC)

Edit (optional):

- `config/pc_defaults.ps1`

Or just pass flags when running.

### 3) Start relay (run from your PC)

From PowerShell:

```powershell
cd external\SamsungGear360\ap_device
.\run_from_pc.ps1 -PiHost 10.0.0.210 -PiUser roman
```

It will sync `ap_device/` to the Pi, connect the Pi to the camera Wi‑Fi, start the relay, and print the stream URL.

### 4) View the stream

- Open the printed URL in your viewer, or use FFplay:

```powershell
ffplay -fflags nobuffer -flags low_delay -i "http://<PI_LAN_IP>:7679/livestream_high.avi"
```

### Quick troubleshooting (only the basics)

- **502 / can’t connect**: make sure the Gear 360 is actually in **live view / streaming** mode.
- **Wrong SSID**: run `.\run_from_pc.ps1 -ListWifi` and copy the SSID exactly into `config/camera_wifi.conf`.

