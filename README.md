# Gear360 Viewer

View live video streams from a Samsung Gear 360 camera.

**Example stream URL:** `http://192.168.43.1:7679/livestream_high.avi` (camera hotspot; your relay IP may differ—see `hardware_setup`.)

## Repository layout

| Folder | Purpose |
|--------|---------|
| **`hardware_setup`** | Raspberry Pi **relay** setup: scripts and config so the Pi bridges the camera Wi‑Fi to your LAN and serves the stream over HTTP (see `hardware_setup/README.md`). |
| **`cpp_player/`** | **C++** viewer: OpenGL + FFmpeg, equirectangular / rectilinear projection, optional stitching and `calibration.toml`. Can also launch **ffplay** or **GStreamer** for a plain preview. Build with CMake; details in `cpp_player/README.md`. |
| **`python_player/`** | **Python** viewer (`gear360_viewer.py`): OpenCV-based playback and projection, quick to iterate on. Install deps from `python_player/requirements.txt`. |
| **`camera_calibration/`** | **Calibration tooling**: fit lens parameters from images/video and write `calibration.toml` for `cpp_player` / `python_player` (`camera_calibration/README.md`). |
| **`libs/glfw/`** | **Git submodule**: GLFW, used by `cpp_player` for windowing and OpenGL context. Initialize with `git submodule update --init --recursive` after clone. |

## Quick Start

### Python (easiest)

```bash
cd python_player
pip install -r requirements.txt
python gear360_viewer.py
```

### C++

```bash
cd cpp_player
mkdir build && cd build
cmake ..
cmake --build .
# Windows (Visual Studio): binary is often build/bin/Debug or build/bin/Release
./bin/gear360_viewer
```

### Command line (FFmpeg only)

```bash
ffplay -hide_banner -fflags nobuffer -flags low_delay -framedrop \
  -i "http://192.168.43.1:7679/livestream_high.avi"
```

**Important:** To close the stream cleanly when using `ffplay`:

- Press `q` to quit, or `Ctrl+C` to interrupt.
- If the stream stays connected, wait a few seconds or restart the camera.

## Python viewer

**Entry point:** `python_player/gear360_viewer.py`

**Install:**

```bash
pip install -r python_player/requirements.txt
```

**Run (example):**

```bash
python python_player/gear360_viewer.py [url] [--fps 30] [--scale 0.5]
```

**Controls:** `q` or `ESC` to quit (see script help for full options).

## C++ viewer

**Location:** `cpp_player/`

**Dependencies (macOS example):**

```bash
brew install ffmpeg glfw glew
```

On Windows, use **vcpkg**, **MSYS2**, or your own FFmpeg / GLFW install paths as described in `cpp_player/CMakeLists.txt`.

**Build:**

```bash
cd cpp_player
mkdir build && cd build
cmake ..
cmake --build .
```

**Run:**

```bash
./bin/gear360_viewer [options] [url]
```

Full flags, config files (`viewer.toml`, `calibration.toml`), and keyboard shortcuts are documented in **`cpp_player/README.md`**.

**Controls (OpenGL window):** `ESC` to quit; `R` reloads calibration when a calibration file is in use.

## Troubleshooting

- **Stream won’t connect:** Confirm the camera is streaming and the URL matches your setup (direct hotspot vs Pi relay in `hardware_setup`).
- **Python:** Ensure dependencies install cleanly: `pip install -r python_player/requirements.txt`.
- **C++ build fails:** Install FFmpeg, GLFW, and GLEW for your platform; see `cpp_player/README.md` and `cpp_player/CMakeLists.txt`.
