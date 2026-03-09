---
name: samsung-gear360
description: Samsung Gear 360 stream setup, Pi relay, and viewers (Python/C++). Use when working on Gear 360 live stream, ap_device relay, gear360_viewer, run_from_pc, viewer.toml, camera_wifi, or stream URL/502/timeout issues.
---

# Samsung Gear 360 – project knowledge

## What this project does

- **Gear 360** camera exposes a live MJPEG stream over its own WiFi AP at `http://192.168.43.1:7679/livestream_high.avi` (camera must be in live view).
- **Pi relay** (`ap_device/`): Raspberry Pi joins the camera WiFi, pulls that stream, and re-serves it on the Pi’s LAN IP so the PC can use `http://<PI_IP>:7679/livestream_high.avi` without being on the camera WiFi.
- **Viewers**: Python (`python/gear360_viewer.py`) and C++ (`cpp/` OpenGL app) can show the stream; both support launching **ffplay** or **GStreamer** as external players.

## Config files (where to change things)

| File | Purpose |
|------|--------|
| `ap_device/config/camera_wifi.conf` | SSID + password for the camera’s WiFi (e.g. `Gear 360(B8F1)`). Synced to Pi. |
| `ap_device/config/pc_defaults.ps1` | `PI_HOST`, `PI_USER`, `PI_AP_DEVICE_PATH` for `run_from_pc.ps1`. |
| `cpp/viewer.toml` | Stream URL for C++ app: `[stream]` with `ip`, `port`, `path`. Default URL is built as `http://<ip>:<port><path>`. |
| `cpp/calibration.toml` | Lens/stitch calibration for C++ OpenGL viewer (lens1, lens2, rotation, alignment). |

## Stream URL and relay

- **Direct to camera (same WiFi):** `http://192.168.43.1:7679/livestream_high.avi`
- **Via Pi relay (from PC):** `http://<PI_IP>:7679/livestream_high.avi` (e.g. `http://10.0.0.210:7679/livestream_high.avi`)
- **Relay** (`ap_device/relay_stream.py`): Must **strip `Content-Length`** when forwarding (camera sends 2^64-1; otherwise FFmpeg reports “Stream ends prematurely”). Sends `Host` and `User-Agent: Samsung Gear 360 Manager/1.0` so the camera accepts the request. Responds with **502** if it cannot reach the camera (Pi not on camera WiFi or camera not streaming). Adds **`X-Relay-Version: 2`** so clients can confirm the updated relay is running.
- **Camera returns 400** for **HEAD**; only **GET** works. Python viewer’s HTTP probe uses GET; treat 206 as success.

## Python viewer (`python/gear360_viewer.py`)

- **Backends:** OpenCV (default), `--ffplay`, `--gstreamer`. No stream check when using `--ffplay`/`--gstreamer`; it just launches the player.
- **ffplay:** Resolved via `shutil.which("ffplay")` or, on Windows, WinGet path `%LOCALAPPDATA%\Microsoft\WinGet\Packages\Gyan.FFmpeg.Essentials_*\ffmpeg-*-essentials_build\bin\ffplay.exe`.
- **GStreamer on Windows:** `gst-launch-1.0` pipeline parsing fails (syntax error with `!`). On Windows, `--gstreamer` **runs ffplay** instead. On Linux/macOS, GStreamer uses in-process PyGObject if available, else `gst-launch-1.0` with pipeline string.
- **In-file GStreamer buffer:** `GST_QUEUE_MAX_BUFFERS`, `GST_QUEUE_MAX_TIME_MS` at top of file (no CLI options).

## C++ viewer (`cpp/`)

- **URL source:** Load `viewer.toml` at startup via `loadStreamConfig("viewer.toml")`; builds URL from `[stream]` ip/port/path. Command-line URL overrides.
- **External player:** `--ffplay` or `--gstreamer` spawns that player with the current URL and exits. **Default external player is ffplay.** On Windows, `--gstreamer` also runs ffplay (same pipeline issue as Python).
- **OpenGL path:** Internal FFmpeg decode + calibration/rectilinear/equirectangular; URL from viewer.toml or argv.
- **`main.cpp` structure:** `parseArguments(argc, argv)` does `loadStreamConfig`, arg parsing, calibration loading — returns `-1` for `--help` (exit 0), `1` for error, `0` for continue. Static bools `s_useFfplay`/`s_useGstreamer` carry external-player flags. `main()` calls `parseArguments`, then checks those bools (external player path bypasses OpenGL entirely), then calls `printStartupInfo()` and enters GLFW/render loop.

## Pi / ap_device

- **Sync:** `run_from_pc.ps1` copies `ap_device` to Pi `~/ap_device` (scp). After sync it runs **CRLF fix** on the Pi (`tr -d '\r'` on .sh/.py and config) so scripts execute.
- **Connect WiFi:** `connect_camera_wifi.sh` uses `nmcli` (with **sudo**). It **skips** if already on same SSID or already on **192.168.43.x**. If exact SSID not in scan, it does a **fuzzy match** (any SSID containing “Gear” and “360”) and connects to that, suggesting to update config. Rescans before connect.
- **Relay:** `start_relay.sh` does **pkill -9 -f relay_stream.py**, sleeps 2s, then starts `python3 relay_stream.py --port 7679`. `run_from_pc.ps1` runs an **explicit kill + sleep** over SSH before calling `start_relay.sh` so the previous relay is gone.
- **Verify relay:** After start, script can check for **X-Relay-Version: 2** on the stream URL (HEAD). If missing, warn that the relay may be an old build.

## Common errors and fixes

| Symptom | Likely cause | Fix |
|--------|----------------|-----|
| 502 Bad Gateway | Relay up but can’t reach camera | Pi on camera WiFi; camera in live view. Run `run_from_pc.ps1` without `-SkipWifi`, or on Pi: `./connect_camera_wifi.sh` then `bash start_relay.sh 7679`. |
| Connection timed out | Nothing on port or relay blocking on camera | Start relay; reduce relay upstream timeout (e.g. 4s) so 502 returns quickly. Check firewall PC→Pi:7679. |
| Stream ends prematurely / nan | Content-Length forwarded from camera | Relay must **not** forward `Content-Length` (skip in header list). Confirm with `curl -sI` → `X-Relay-Version: 2`. |
| No network with SSID 'Gear 360(B8F1)' found | SSID mismatch (e.g. space before paren) | Use `-ListWifi` to see exact SSID; update `camera_wifi.conf`. Connect script also tries fuzzy “Gear”+“360” match. If stream already works, use `-SkipWifi`. |
| GStreamer “erroneous pipeline: syntax error” (Windows) | gst-launch-1.0 and `!` in pipeline on Windows | Use ffplay for playback on Windows (Python and C++ both map `--gstreamer` to ffplay on Windows). |
| ffplay not found (Windows) | FFmpeg not on PATH | Viewer looks in WinGet path for Gyan.FFmpeg.Essentials; or add GStreamer/FFmpeg bin to PATH. |

## Paths (key files)

- **PC automation:** `ap_device/run_from_pc.ps1`
- **Pi scripts (all at `ap_device/` root — must stay there; run_from_pc.ps1 uses `cd ~/ap_device && ./script`):**
  - `connect_camera_wifi.sh` — join camera WiFi
  - `start_relay.sh` — launch relay in background (called by orchestrator)
  - `relay_stream.py` — the relay server itself
  - `list_wifi.sh` — list visible WiFi networks (debug SSID mismatches; invoked via `-ListWifi` flag)
- **Viewers:** `python/gear360_viewer.py`, `cpp/main.cpp`
- **Stream config:** `cpp/viewer.toml`, `ap_device/config/camera_wifi.conf`

## One-liner workflow (from PC)

```powershell
cd SamsungGear360\ap_device
.\run_from_pc.ps1
# Then open stream (Python):  python ..\python\gear360_viewer.py --ffplay http://10.0.0.210:7679/livestream_high.avi
# Or C++:  Gear360Viewer --ffplay   (URL from cpp/viewer.toml)
```
