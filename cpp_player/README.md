# Samsung Gear 360 Viewer

Live stream viewer for the Samsung Gear 360 in a standalone executable. Source code is here for ease of porting to other projects.

### Build

```bash
cd cpp_player
mkdir build && cd build
cmake ..
cmake --build .
```

On Windows with the Visual Studio generator, the executable is often under `build/bin/Debug` or `build/bin/Release`. On Unix, typically `build/bin/gear360_viewer`.

### Command-line usage

```text
gear360_viewer [options] [url]
```

Run `gear360_viewer --help` for the same summary.

| Option | Description |
|--------|-------------|
| `[url]` | Stream URL (optional). If omitted, the URL is built from `viewer.toml` `[stream]` (`ip`, `port`, `path`). |
| `--help`, `-h` | Print options and exit. |
| `--ffplay` | Play with ffplay (external FFmpeg player) instead of the OpenGL viewer. |
| `--gstreamer` | Play with GStreamer (`gst-launch-1.0`). On Windows, if GStreamer fails, falls back to ffplay. |
| `--rectilinear` | OpenGL: spherical-to-flat rectilinear view. |
| `--equirectangular` | OpenGL: dual fisheye to equirectangular (360°) view. |
| `--fov <degrees>` | Field of view for `--rectilinear` / `--equirectangular` (must be `0 < fov ≤ 360`). If you enable either mode and omit `--fov`, the default is **195**. |
| `--stitch` | Stitch mode for equirectangular projection (use with `--equirectangular`). |
| `--calibration <file>` | Load lens calibration from a TOML file. If `--rectilinear` / `--equirectangular` are not set, equirectangular mode is turned on automatically when calibration loads successfully. |
| `--light-falloff` | Enable lens light falloff compensation in the shader. |

**Examples**

```bash
# OpenGL viewer; URL from viewer.toml
./gear360_viewer

# Explicit stream URL
./gear360_viewer http://10.0.0.210:7679/livestream_high.avi

# External players (URL from viewer.toml or pass [url] as above)
./gear360_viewer --ffplay
./gear360_viewer --gstreamer

# OpenGL projection modes
./gear360_viewer --rectilinear
./gear360_viewer --equirectangular --stitch --fov 195
./gear360_viewer --calibration calibration.toml http://10.0.0.210:7679/livestream_high.avi
```

### Config files

- **`viewer.toml`** — Stream URL (`[stream]`), and optional `[external_player]` paths (`ffplay`, `gst_launch`, GStreamer queue/sync options) used by `--ffplay` / `--gstreamer`.
- **`calibration.toml`** — Default calibration path when you do not pass `--calibration` (resolved next to the executable / working directory). See `--calibration` above.

### OpenGL viewer keyboard shortcuts

| Key | Action |
|-----|--------|
| **R** | Reload calibration from the active calibration file (same path as at startup). Save `calibration.toml` first. |
| **Esc** | Quit. |

Diagnostics are also written to `gear360_viewer.log` next to the executable when logging is enabled.
