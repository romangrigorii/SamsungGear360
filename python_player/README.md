# Samsung Gear 360 — Python Tools

Viewer commands run from `python_player/`. Calibration commands run from the repo root.

---

## Live Viewer

```powershell
python gear360_viewer.py                          # uses viewer.toml [stream]
python gear360_viewer.py http://10.0.0.210:7679/livestream_high.avi
```

**Keys while viewing:**

| Key | Action |
|-----|--------|
| `1` / `2` / `3` | Fisheye, rectilinear, or 360 (same as the top bar) |
| drag / scroll | In 360 only: look around, change field of view |
| `t` | Stitch on or off. Off by default. Needs `--calibration` |
| `r` | Start recording — title bar changes to `[REC]` |
| `r` (again) | Stop recording — file is saved |
| `s` | Screenshot of the current frame |
| `q` / ESC | Quit (stops any active recording) |

Stitch uses the calibration file on every frame (`stitch_dual_fisheye`), so the seam does not drift. Rectilinear stays looking forward. 360 feeds yaw and pitch from the mouse into the same projection.

Recordings and screenshots are saved to `python_player/saved_files/` with names like  
`gear360_20260427_183052.mp4` / `.png` — timestamped at the moment of completion.

**Useful flags:**

```powershell
--resolution low       # camera stream: high (default) or low
--size 1280x640        # decoder frame size; does not change the camera
--scale 0.5            # display at half resolution
--calibration path     # load calibration.toml
--ignore-stream-check  # skip the pre-flight HEAD check
--ffplay               # use external ffplay window instead
```

All flags can also be set permanently in `python_player/viewer.toml`.

To write a stitched file instead of viewing live:

```powershell
python camera_calibration/realtime/stitcher.py clip.mp4 --calibration calibration.toml --output_format 360 -o clip_360.mp4
```

`rectalinear` writes the flat view. Details are in `camera_calibration/README.md`.

---

## Calibration

### What you need

A dual-fisheye image or video from the Gear 360 with visible texture at the seam.  
No checkerboard required — the optimizer minimizes pixel difference at the stitch seam.

### Step 1 — Run calibration and write TOML in one step

```powershell
# From an image
python camera_calibration/calib/create_calibration.py --image frame.jpg -o calibration.toml

# From a video (samples 5 evenly-spaced frames)
python camera_calibration/calib/create_calibration.py --video clip.avi -o calibration.toml

# Joint optimizer (slower, sometimes more accurate)
python camera_calibration/calib/create_calibration.py --video clip.avi -o calibration.toml --solver adjoint
```

The TOML format matches `cpp/calibration.toml` and is read by both viewers.  
Add `--save-json result.json` to also keep the raw optimizer output.

**Tuning which parameters are optimized** — each solver has its own config in
`camera_calibration/solvers/configs/`:

- `adjoint_tuning.toml` — used by `--solver adjoint`. Lens parameter bounds,
  preprocessing (vignette / edges / seam strip), and regularization weights.
- `features_tuning.toml` — used by `--solver features`. Lens parameter bounds,
  feature detector / Lowe ratio / RANSAC tolerance, and per-parameter Tikhonov
  weights.
- `plumbline_tuning.toml` — used by `--solver plumbline`. Line-segment
  detector settings, plus the same `[lens*]` / `[regularization]` schema
  (rotations default to `optimize = false` because they're unobservable from
  plumb-line data).

All files share the `[lens1.*]` / `[lens2.*]` schema (`optimize = true/false`,
`nominal`, `min`, `max`, `use_lens1`). Each is auto-loaded for its solver;
override any with `--config <path>`.

### Convert an existing optimizer JSON

```powershell
python camera_calibration/calib/create_calibration.py --from-json result.json -o calibration.toml
```

### Baseline TOML without an image

```powershell
python camera_calibration/calib/create_calibration.py --preset gear360 -o calibration.toml
```

### Step 2 — Use it

```powershell
# Pass directly
python gear360_viewer.py --calibration calibration.toml

# Or set permanently in viewer.toml
calibration = "calibration.toml"
```

---

## Calibration parameters (per lens)

| Parameter | Description | Unit |
|-----------|-------------|------|
| `center_x / center_y` | Optical center | normalized 0–1 |
| `fov` | FOV scale factor (1.0 = 180°, 1.083 = 195°) | scale |
| `p1 / p2 / p3` | Radial distortion coefficients | — |
| `rotation_yaw/pitch/roll` | Lens tilt | degrees |
| `offset_x / offset_y` | Sub-pixel alignment nudge | normalized 0–1 |
