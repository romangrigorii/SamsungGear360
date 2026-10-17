#!/usr/bin/env python3
"""
Gear 360 live-stream viewer.

Default (http/https + ffmpeg on PATH): FFmpeg decodes in a background thread → 1-frame
queue → OpenCV imshow.  The UI thread only calls waitKey so the window stays responsive
even on slow/WiFi relays.

The top bar selects Fisheye, Rectilinear, or 360. Rectilinear and 360 call
camera_calibration/projections. 360 drags that rectilinear view (yaw and pitch).
Stitch starts off. Turn it on and those views use the calibration file with
stitch_dual_fisheye. Keys: 1 fisheye, 2 rectilinear, 3 360, t stitch.

Backends (pick one):
  default       FFmpeg pipe + OpenCV  (auto-selected for http when ffmpeg is available)
  --ffplay      external ffplay window
  --gstreamer   gst-launch-1.0 (Linux) / ffplay (Windows)
  --opencv      OpenCV VideoCapture(FFMPEG) in the main thread
"""

import argparse
import logging
import math
import os
import queue
import shutil
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

import cv2
import numpy as np

logger = logging.getLogger(__name__)

_DEFAULT_RELAY_URL  = "http://10.0.0.210:7679/livestream_high.avi"
_CAMERA_AP_URL      = "http://192.168.43.1:7679/livestream_high.avi"
_FFMPEG_ANALYZE_US  = "2000000"
_FFMPEG_PROBE_SIZE  = "1048576"
_FFPROBE_TIMEOUT    = 8    # seconds per ffprobe attempt
_GEAR360_W, _GEAR360_H = 1280, 640   # measured high stream; override with --size
# Live view only offers these two paths. The camera does not accept a custom WxH.
_STREAM_PATHS = {
    "high": "/livestream_high.avi",
    "low": "/livestream_low.avi",
}


# ---------------------------------------------------------------------------
# Config / TOML helpers
# ---------------------------------------------------------------------------

def _script_dir() -> str:
    return os.path.dirname(os.path.abspath(__file__))


def _find_viewer_toml() -> str | None:
    """python/viewer.toml takes priority; cpp/viewer.toml is the shared fallback."""
    here = _script_dir()
    for p in (
        os.path.join(here, "viewer.toml"),           # python/viewer.toml  ← checked first
        os.path.join(here, "..", "cpp", "viewer.toml"),
        os.path.join(os.getcwd(), "viewer.toml"),
    ):
        p = os.path.normpath(p)
        if os.path.isfile(p):
            return p
    return None


def _parse_toml_loose(path: str) -> dict:
    out: dict = {}
    section = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1].strip()
                out.setdefault(section, {})
                continue
            if not section or "=" not in line:
                continue
            k, v = line.split("=", 1)
            k, v = k.strip(), v.strip()
            if len(v) >= 2 and v[0] == v[-1] and v[0] in ('"', "'"):
                v = v[1:-1]
            out[section][k] = v
    return out


def load_viewer_toml_config() -> dict | None:
    p = _find_viewer_toml()
    if not p:
        return None
    try:
        t = _parse_toml_loose(p)
    except OSError:
        return None
    s = t.get("stream", {})
    ip   = s.get("ip", "10.0.0.210")
    path = s.get("path", "/livestream_high.avi") or "/livestream_high.avi"
    if not path.startswith("/"):
        path = "/" + path
    try:
        port = int(s.get("port", 7679))
    except (TypeError, ValueError):
        port = 7679
    ep = t.get("external_player", {})
    try:
        gqb = int(ep.get("gst_queue_max_buffers", 1) or 1)
    except ValueError:
        gqb = 1
    try:
        gqt = int(ep.get("gst_queue_max_time_ms", 0) or 0)
    except ValueError:
        gqt = 0
    return {
        "toml_path": p,
        "url": f"http://{ip}:{port}{path}",
        "ffplay": (ep.get("ffplay") or "").strip() or None,
        "gst_launch": (ep.get("gst_launch") or "").strip() or None,
        "gst_queue_max_buffers": gqb,
        "gst_queue_max_time_ms": gqt,
    }


def load_viewer_defaults() -> dict:
    """
    Read [viewer] section from python/viewer.toml.
    Returns config values; every key maps to the same name as the CLI flag.
    """
    p = _find_viewer_toml()
    # Hardcoded fallbacks used when the key is absent from the config file.
    d: dict = {
        "scale": 1.0, "fps": 0, "size": "",
        "resolution": "",
        "log_level": "INFO", "log_file": "",
        "ffplay": False, "gstreamer": False,
        "ffmpeg_opencv": False, "opencv": False,
        "calibration": "", "ignore_stream_check": False,
    }
    if not p:
        return d
    try:
        t = _parse_toml_loose(p)
    except OSError:
        return d
    v = t.get("viewer", {})
    for key, cast in (("scale", float), ("fps", int)):
        if key in v:
            try:
                d[key] = cast(v[key])
            except (TypeError, ValueError):
                pass
    for key in ("size", "log_file", "calibration", "resolution"):
        if key in v:
            d[key] = str(v[key]).strip().strip('"').strip("'")
    if d["resolution"].lower() not in ("", "high", "low"):
        d["resolution"] = ""
    else:
        d["resolution"] = d["resolution"].lower()
    lvl = str(v.get("log_level", "INFO")).upper()
    if lvl in ("DEBUG", "INFO", "WARNING", "ERROR"):
        d["log_level"] = lvl
    for key in ("ffplay", "gstreamer", "ffmpeg_opencv", "opencv", "ignore_stream_check"):
        if key in v:
            d[key] = str(v[key]).lower().strip() in ("true", "1", "yes")
    return d


def default_stream_url() -> str:
    cfg = load_viewer_toml_config()
    return cfg["url"] if cfg else _DEFAULT_RELAY_URL


def _find_calibration_toml(cli_path: str = "") -> str:
    """Return an absolute path to a calibration.toml, or "" if none found.

    Resolution order:
      1. Explicit path (CLI --calibration or viewer.toml calibration key)
         resolved relative to viewer.toml directory, then script directory, then cwd.
      2. calibration.toml adjacent to viewer.toml.
      3. cpp/calibration.toml (fallback for C++-only setups).
    """
    here = os.path.dirname(os.path.abspath(__file__))
    v = _find_viewer_toml()
    toml_dir = os.path.dirname(v) if v else here

    if cli_path:
        for base in (toml_dir, here, os.getcwd()):
            p = os.path.normpath(os.path.join(base, cli_path))
            if os.path.isfile(p):
                return p
        if os.path.isfile(cli_path):
            return os.path.normpath(cli_path)
        return ""

    for c in (
        os.path.normpath(os.path.join(toml_dir, "calibration.toml")),
        os.path.normpath(os.path.join(here, "..", "cpp", "calibration.toml")),
        os.path.normpath(os.path.join(here, "..", "cpp_player", "calibration.toml")),
        os.path.normpath(os.path.join(here, "calibration.toml")),
    ):
        if os.path.isfile(c):
            return c
    return ""


def load_calibration_toml(path: str) -> dict:
    """Load a calibration.toml and return the parsed dict (lens1/lens2 sections)."""
    try:
        t = _parse_toml_loose(path)
    except OSError:
        return {}
    if "lens1" not in t and "lens2" not in t:
        return {}
    return t


def _toml_to_camera_calibration(toml_dict: dict):
    """Convert a parsed calibration.toml dict to a CameraCalibration object.

    TOML stores: optional [metadata] fov_deg (degrees) for both lenses; else [lens] fov as scale
    (<10) or degrees. Rotations in degrees; distortion as p1/p2/p3.
    CameraCalibration expects: fov in degrees, rotations in radians, distortion as k1/k2/k3.
    """
    try:
        here = os.path.dirname(os.path.abspath(__file__))
        root = os.path.normpath(os.path.join(here, ".."))
        if here not in sys.path:
            sys.path.insert(0, here)
        if root not in sys.path:
            sys.path.insert(0, root)
        from camera_calibration.calib.calibration_config import CameraCalibration, LensCalibration
    except ImportError as e:
        logger.error("Cannot import calibration module: %s", e)
        return None

    meta = toml_dict.get("metadata") or {}
    shared_fov_deg = None
    if meta.get("fov_deg") is not None:
        shared_fov_deg = float(meta["fov_deg"])
    elif meta.get("lens_fov_deg") is not None:
        shared_fov_deg = float(meta["lens_fov_deg"])

    def _lens(section: dict):
        if shared_fov_deg is not None:
            fov_deg = shared_fov_deg
        else:
            fov_raw = float(section.get("fov", 1.0833))
            fov_deg = fov_raw * 180.0 if fov_raw < 10.0 else fov_raw
        return LensCalibration(
            center_x      = float(section.get("center_x", 0.5)),
            center_y      = float(section.get("center_y", 0.5)),
            fov           = fov_deg,
            k1            = float(section.get("p1", 0.0)),
            k2            = float(section.get("p2", 0.0)),
            k3            = float(section.get("p3", 0.0)),
            rotation_yaw  = math.radians(float(section.get("rotation_yaw",   0.0))),
            rotation_pitch= math.radians(float(section.get("rotation_pitch", 0.0))),
            rotation_roll = math.radians(float(section.get("rotation_roll",  0.0))),
        )

    try:
        lens1 = _lens(toml_dict.get("lens1", {}))
        lens2 = _lens(toml_dict.get("lens2", {}))
        return CameraCalibration(lens1=lens1, lens2=lens2)
    except Exception as e:
        logger.error("Failed to build CameraCalibration: %s", e)
        return None


class _EquirectProjector:
    """Pre-computes remap maps for both lenses; applies them with cv2.remap each frame.

    Map computation (numpy trig) runs once at startup.  Per-frame cost is two
    cv2.remap calls (fast C++) plus a horizontal stack — suitable for live use.
    """

    def __init__(self, calib, frame_width: int, frame_height: int, base_fov_deg: float = 195.0):
        import numpy as np

        lw, lh = frame_width // 2, frame_height   # per-lens input dimensions
        out_w   = frame_width                      # output same width as input for performance
        out_h   = frame_height
        half_ow = out_w // 2

        lens_fov_deg = min(calib.lens1.fov, calib.lens2.fov)
        proj_w = int(half_ow * lens_fov_deg / base_fov_deg)
        self._overlap = proj_w - half_ow
        self._half_ow = half_ow
        self._out_w   = out_w
        self._out_h   = out_h

        self._xmap_l, self._ymap_l, self._valid_l = self._build_maps(
            calib.lens1, lw, lh, proj_w, out_h, lens_fov_deg)
        self._xmap_r, self._ymap_r, self._valid_r = self._build_maps(
            calib.lens2, lw, lh, proj_w, out_h, lens_fov_deg)

    @staticmethod
    def _build_maps(lens, img_w, img_h, proj_w, proj_h, lens_fov_deg):
        import numpy as np
        cx0    = lens.center_x * img_w
        cy0    = lens.center_y * img_h
        radius = min(img_w, img_h) / 2.0

        v, u = np.mgrid[0:proj_h, 0:proj_w]
        lon_range = np.radians(lens_fov_deg)
        lon  = (u / proj_w) * lon_range - lon_range / 2
        lat  = (0.5 - v / proj_h) * math.pi

        Xw = np.cos(lat) * np.cos(lon)
        Yw = np.cos(lat) * np.sin(lon)
        Zw = np.sin(lat)

        yaw, pitch, roll = lens.rotation_yaw, lens.rotation_pitch, lens.rotation_roll
        if abs(yaw) > 1e-6 or abs(pitch) > 1e-6 or abs(roll) > 1e-6:
            cy, sy = math.cos(yaw),   math.sin(yaw)
            cp, sp = math.cos(pitch), math.sin(pitch)
            cr, sr = math.cos(roll),  math.sin(roll)
            Rz = np.array([[cy, -sy, 0], [sy,  cy, 0], [0, 0, 1]], np.float64)
            Ry = np.array([[cp,  0, sp], [ 0,   1, 0], [-sp,0,cp]], np.float64)
            Rx = np.array([[ 1,  0,  0], [ 0,  cr,-sr], [0, sr,cr]], np.float64)
            world = np.stack([Xw, Yw, Zw], axis=-1)
            rot   = world @ (Rz @ Ry @ Rx).T
            Xw, Yw, Zw = rot[..., 0], rot[..., 1], rot[..., 2]

        Px, Py, Pz = -Zw, Yw, Xw
        phi   = np.arccos(np.clip(Pz, -1, 1))
        theta = np.arctan2(Py, Px) - math.pi / 2
        r     = phi / math.radians(lens_fov_deg / 2)

        if abs(lens.k1) > 1e-6 or abs(lens.k2) > 1e-6 or abs(lens.k3) > 1e-6:
            r = r * (1.0 + lens.k1 * r**2 + lens.k2 * r**4 + lens.k3 * r**6)

        xmap = (cx0 + r * np.cos(theta) * radius).astype(np.float32)
        ymap = (cy0 - r * np.sin(theta) * radius).astype(np.float32)
        valid = (r <= 1.001)
        return xmap, ymap, valid

    def project(self, frame):
        """Split frame into two halves, remap, and stitch to equirectangular."""
        import numpy as np
        h, w = frame.shape[:2]
        left  = frame[:, :w // 2]
        right = frame[:, w // 2:]
        right_flip = np.fliplr(right)

        lp = cv2.remap(left,       self._xmap_l, self._ymap_l,
                       cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT)
        lp[~self._valid_l] = 0

        rp = cv2.remap(right_flip, self._xmap_r, self._ymap_r,
                       cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT)
        rp[~self._valid_r] = 0
        rp = np.fliplr(rp)

        ov = self._overlap
        hw = self._half_ow
        if ov <= 0:
            return np.hstack([lp, rp])

        # Alpha-blend the overlap band; take unblended centre slices
        alpha  = np.linspace(1, 0, ov, dtype=np.float32)[None, :, None]
        c_blend = (lp[:, -ov:].astype(np.float32) * alpha +
                   rp[:, :ov ].astype(np.float32) * (1 - alpha)).astype(np.uint8)
        s_blend = (lp[:, :ov ].astype(np.float32) * (1 - alpha) +
                   rp[:, -ov:].astype(np.float32) * alpha).astype(np.uint8)

        return np.hstack([s_blend,
                          lp[:, ov:hw],
                          c_blend,
                          rp[:, ov:hw],
                          s_blend])


# ---------------------------------------------------------------------------
# View menu: fisheye, rectilinear, draggable 360, realtime stitch
# ---------------------------------------------------------------------------

_FLAT_FOV = 90.0
_VIEW_LABELS = {"fisheye": "Fisheye", "rectilinear": "Rectilinear", "360": "360"}


class _ViewSession:
    """Menu over the projection code that already exists.

    Fisheye is the camera frame. Rectilinear and 360 call
    ``extract_rectilinear_view``. 360 passes the drag yaw and pitch into that
    function. Stitch starts off. While it is on, those two views use
    ``stitch_dual_fisheye`` with the loaded calibration, then
    ``equirect_to_rectilinear``. The lens model is fixed, so the seam does not move.
    """

    def __init__(self, lens_fov_deg: float = 195.0, calibration=None) -> None:
        self.lens_fov = float(lens_fov_deg) if float(lens_fov_deg) > 0 else 195.0
        self.calibration = calibration
        self.mode = "fisheye"
        self.stitch_enabled = False
        self.recording = False
        self.yaw = 0.0
        self.pitch = 0.0
        self.look_fov = _FLAT_FOV
        self.estimate = None
        self.needs_redraw = False
        self._dragging = False
        self._last_xy: tuple[int, int] | None = None
        self._hits: list[tuple] = []
        self._bar_h = 36
        self._disp_w = 1
        self._disp_h = 1
        self._title = ""
        self._render_error = ""
        self._stitch_error = ""
        self._rectilinear = None
        self._equirect_view = None
        self._center_forward = None
        self._stitch_dual = None
        self._mask_disc = None
        self._load_views()

    def _load_views(self) -> None:
        root = os.path.normpath(os.path.join(_script_dir(), ".."))
        if root not in sys.path:
            sys.path.insert(0, root)
        try:
            from camera_calibration.projections.fisheye_to_rectilinear import extract_rectilinear_view
            self._rectilinear = extract_rectilinear_view
        except Exception as e:
            logger.error("Rectilinear projection unavailable: %s", e)
        try:
            from camera_calibration.projections.fisheye_to_equirectangular import (
                mask_fisheye_circle,
                stitch_dual_fisheye,
            )
            from camera_calibration.realtime.stitcher import _center_forward, equirect_to_rectilinear

            self._stitch_dual = stitch_dual_fisheye
            self._mask_disc = mask_fisheye_circle
            self._equirect_view = equirect_to_rectilinear
            self._center_forward = _center_forward
        except Exception as e:
            self._stitch_error = str(e)
            logger.error("Calibrated stitch unavailable: %s", e)

    def set_mode(self, mode: str) -> None:
        if mode not in _VIEW_LABELS or mode == self.mode:
            return
        self.mode = mode
        self._dragging = False
        logger.info("View: %s", _VIEW_LABELS[mode])
        self.update_title()
        self.needs_redraw = True

    def toggle_stitch(self) -> None:
        if not self.stitch_enabled:
            if self.calibration is None:
                logger.error("Stitch needs a calibration file. Pass --calibration.")
                return
            if self._stitch_dual is None:
                logger.error("Calibrated stitch unavailable: %s", self._stitch_error or "import failed")
                return
        self.stitch_enabled = not self.stitch_enabled
        self.estimate = None
        logger.info("Calibrated stitch %s", "on" if self.stitch_enabled else "off")
        self.update_title()
        self.needs_redraw = True

    def set_recording(self, recording: bool) -> None:
        if recording == self.recording:
            return
        self.recording = recording
        self.update_title()

    def nudge(self, dyaw: float, dpitch: float) -> None:
        if self.mode != "360":
            return
        self.yaw = (self.yaw + dyaw) % 360.0
        self.pitch = float(min(85.0, max(-85.0, self.pitch + dpitch)))
        self.needs_redraw = True

    def update_title(self) -> None:
        view = _VIEW_LABELS[self.mode]
        stitch = "stitch on" if self.stitch_enabled else "stitch off"
        if self.recording:
            title = (
                f"Gear360 Viewer  |  [REC]  {view}  {stitch}"
                "  |  r: stop   s: screenshot   q: quit"
            )
        else:
            extra = "   drag to look" if self.mode == "360" else ""
            title = (
                f"Gear360 Viewer  |  {view}  {stitch}{extra}"
                "  |  r: record   s: screenshot   q: quit"
            )
        if title == self._title:
            return
        self._title = title
        try:
            cv2.setWindowTitle(_WNAME, title)
        except cv2.error:
            pass

    def _look(self) -> tuple[float, float, float]:
        if self.mode == "360":
            return self.yaw, self.pitch, self.look_fov
        return 0.0, 0.0, _FLAT_FOV

    def render(self, frame: np.ndarray) -> np.ndarray:
        """Return a frame the same size as ``frame``, without the menu bar."""
        if frame.ndim == 2:
            frame = cv2.cvtColor(frame, cv2.COLOR_GRAY2BGR)
        height, width = frame.shape[:2]
        if self.mode == "fisheye":
            self.estimate = None
            return frame
        yaw, pitch, fov = self._look()
        try:
            if self.stitch_enabled and self._stitch_dual is not None and self.calibration is not None:
                half = width // 2
                left, _left_mask = self._mask_disc(frame[:, :half])
                right, _right_mask = self._mask_disc(frame[:, half : half * 2])
                equirect, _lp, _rp, _overlap = self._stitch_dual(
                    left, right, self.calibration, 180.0, width if width % 2 == 0 else width - 1,
                    (width if width % 2 == 0 else width - 1) // 2, True,
                )
                equirect = self._center_forward(equirect)
                view = self._equirect_view(equirect, width, height, fov, yaw, pitch)
            elif self._rectilinear is not None:
                self.estimate = None
                view, _valid = self._rectilinear(
                    frame, width, height, fov, yaw, pitch,
                    self.lens_fov, self.calibration, True,
                )
            else:
                self.estimate = None
                return frame
            self._render_error = ""
            return view
        except Exception as e:
            msg = str(e)
            if msg != self._render_error:
                logger.error("View failed (%s): %s", self.mode, e)
                self._render_error = msg
            self.estimate = None
            return frame

    def paint(self, disp: np.ndarray) -> None:
        """Draw the view menu onto the display image and record click targets."""
        h, w = disp.shape[:2]
        self._disp_w = max(1, w)
        self._disp_h = max(1, h)
        bar_h = 36 if h >= 96 else max(20, h // 4)
        self._bar_h = bar_h
        strip = disp[:bar_h]
        dark = np.empty_like(strip)
        dark[:] = (16, 16, 16)
        cv2.addWeighted(dark, 0.78, strip, 0.22, 0, strip)
        cv2.line(disp, (0, bar_h - 1), (w - 1, bar_h - 1), (90, 90, 90), 1)

        labels = [
            ("view", "fisheye", "Fisheye"),
            ("view", "rectilinear", "Rectilinear"),
            ("view", "360", "360"),
            ("stitch", "", "Stitch: " + ("On" if self.stitch_enabled else "Off")),
        ]
        scale = 0.48
        gap = 8
        pad = 12
        widths: list[int] = []
        while True:
            widths = []
            for _kind, _value, text in labels:
                (tw, _th), _ = cv2.getTextSize(text, cv2.FONT_HERSHEY_SIMPLEX, scale, 1)
                widths.append(tw + pad * 2)
            if sum(widths) + gap * (len(labels) + 1) <= w or scale <= 0.34:
                break
            scale -= 0.04

        x = gap
        hits = []
        for (kind, value, text), bw in zip(labels, widths):
            if kind == "view":
                color = (196, 122, 48) if self.mode == value else (52, 52, 52)
            else:
                color = (70, 150, 60) if self.stitch_enabled else (52, 52, 52)
            y0, y1 = 5, max(6, bar_h - 6)
            cv2.rectangle(disp, (x, y0), (x + bw, y1), color, -1)
            cv2.rectangle(disp, (x, y0), (x + bw, y1), (20, 20, 20), 1)
            (tw, th), _ = cv2.getTextSize(text, cv2.FONT_HERSHEY_SIMPLEX, scale, 1)
            tx = x + max(0, (bw - tw) // 2)
            ty = y0 + max(th, (y1 - y0 + th) // 2 - 1)
            cv2.putText(
                disp, text, (tx, ty), cv2.FONT_HERSHEY_SIMPLEX, scale,
                (245, 245, 245), 1, cv2.LINE_AA,
            )
            hits.append((x, y0, x + bw, y1, kind, value))
            x += bw + gap

        hint = ""
        if self.mode == "360":
            hint = "drag to look"
        elif self.stitch_enabled and self.mode == "fisheye":
            hint = "used by Rectilinear and 360"
        if self.stitch_enabled and self.mode != "fisheye":
            hint = "calibration"
        if hint:
            (hw, hh), _ = cv2.getTextSize(hint, cv2.FONT_HERSHEY_SIMPLEX, scale, 1)
            if x + hw + 8 < w:
                cv2.putText(
                    disp, hint, (x + 4, (bar_h + hh) // 2 - 1),
                    cv2.FONT_HERSHEY_SIMPLEX, scale, (220, 220, 220), 1, cv2.LINE_AA,
                )
        self._hits = hits

    def _activate(self, x: int, y: int) -> bool:
        for x0, y0, x1, y1, kind, value in self._hits:
            if x0 <= x < x1 and y0 <= y < y1:
                if kind == "view":
                    self.set_mode(value)
                else:
                    self.toggle_stitch()
                self.needs_redraw = True
                return True
        return False

    def _drag(self, dx: float, dy: float) -> None:
        hfov = self.look_fov
        aspect = self._disp_h / float(self._disp_w)
        vfov = math.degrees(2.0 * math.atan(aspect * math.tan(math.radians(hfov) * 0.5)))
        self.yaw = (self.yaw - dx / self._disp_w * hfov) % 360.0
        self.pitch = float(min(85.0, max(-85.0, self.pitch + dy / self._disp_h * vfov)))

    def on_mouse(self, event, x, y, flags, param=None) -> None:
        if event == cv2.EVENT_LBUTTONDOWN:
            self._dragging = False
            if self._activate(x, y) or y < self._bar_h:
                return
            if self.mode == "360":
                self._dragging = True
                self._last_xy = (x, y)
        elif event == cv2.EVENT_MOUSEMOVE and self._dragging and self._last_xy is not None:
            lx, ly = self._last_xy
            dx, dy = x - lx, y - ly
            if dx == 0 and dy == 0:
                return
            self._last_xy = (x, y)
            self._drag(dx, dy)
            self.needs_redraw = True
        elif event == cv2.EVENT_LBUTTONUP:
            self._dragging = False
            self._last_xy = None
        elif event == cv2.EVENT_MOUSEWHEEL and self.mode == "360":
            step = -4.0 if flags > 0 else 4.0
            self.look_fov = float(min(140.0, max(40.0, self.look_fov + step)))
            self.needs_redraw = True


# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------

def setup_logging(log_file: str, level_name: str) -> None:
    level = getattr(logging, level_name.upper(), logging.INFO)
    root = logging.getLogger()
    root.handlers.clear()
    root.setLevel(level)

    class _Utc(logging.Formatter):
        converter = time.gmtime

    fmt = _Utc("%(asctime)s %(levelname)s %(message)s", "%Y-%m-%dT%H:%M:%SZ")
    sh = logging.StreamHandler(sys.stderr)
    sh.setFormatter(fmt)
    root.addHandler(sh)
    if log_file:
        try:
            fh = logging.FileHandler(log_file, encoding="utf-8")
            fh.setFormatter(fmt)
            root.addHandler(fh)
        except OSError as e:
            print(f"Could not open --log-file {log_file}: {e}", file=sys.stderr)


def _flush_log() -> None:
    for h in logging.getLogger().handlers:
        try:
            h.flush()
        except OSError:
            pass


# ---------------------------------------------------------------------------
# FFmpeg / ffprobe discovery
# ---------------------------------------------------------------------------

def _find_ff(basename: str) -> str | None:
    """Find ffmpeg/ffplay/ffprobe: PATH first, then WinGet FFmpeg Essentials."""
    w = shutil.which(f"{basename}.exe" if sys.platform == "win32" else basename)
    if w:
        return w
    if sys.platform != "win32":
        return None
    local = os.environ.get("LOCALAPPDATA", "")
    pkgs = os.path.join(local, "Microsoft", "WinGet", "Packages") if local else ""
    if not pkgs or not os.path.isdir(pkgs):
        return None
    for name in os.listdir(pkgs):
        if not name.startswith("Gyan.FFmpeg") or "Essentials" not in name:
            continue
        pkg = os.path.join(pkgs, name)
        for sub in os.listdir(pkg) if os.path.isdir(pkg) else ():
            if sub.startswith("ffmpeg-") and "essentials" in sub.lower():
                cand = os.path.join(pkg, sub, "bin", f"{basename}.exe")
                if os.path.isfile(cand):
                    return cand
    return None


# ---------------------------------------------------------------------------
# Stream size detection
# ---------------------------------------------------------------------------

def _looks_like_gear360(url: str) -> bool:
    u = url.lower()
    return "livestream" in u or "192.168.43.1" in u


def apply_stream_resolution(url: str, resolution: str) -> str:
    """Select the camera's high or low live-view path. Other URLs are unchanged."""
    path = _STREAM_PATHS.get((resolution or "").lower())
    if not path:
        return url
    parts = urllib.parse.urlsplit(url)
    if not parts.netloc:
        return url
    return urllib.parse.urlunsplit((parts.scheme, parts.netloc, path, "", ""))


def _ffprobe_dims(url: str) -> tuple[int, int] | None:
    path = _find_ff("ffprobe")
    if not path:
        return None
    cmd = [
        path, "-v", "error",
        "-analyzeduration", _FFMPEG_ANALYZE_US,
        "-probesize", _FFMPEG_PROBE_SIZE,
        "-select_streams", "v:0",
        "-show_entries", "stream=width,height",
        "-of", "csv=p=0:s=x",
        "-i", url,
    ]
    try:
        out = subprocess.check_output(cmd, stderr=subprocess.STDOUT, text=True, timeout=_FFPROBE_TIMEOUT).strip()
        if "x" in out:
            w, h = out.split("x", 1)
            w, h = int(w), int(h)
            if w > 0 and h > 0:
                return w, h
    except Exception as e:
        logger.debug("ffprobe failed: %s", e)
    return None


def resolve_stream_dims(url: str, override: str = "") -> tuple[int, int]:
    """Return (width, height) to use for the BGr24 pipe."""
    if override:
        try:
            w, h = override.lower().split("x", 1)
            return int(w), int(h)
        except ValueError:
            logger.warning("--size %r is not WxH, ignoring.", override)

    if _looks_like_gear360(url) and "livestream_low" not in url.lower():
        logger.info("Gear360 stream — using %dx%d default (--size WxH to override).", _GEAR360_W, _GEAR360_H)
        _flush_log()
        return _GEAR360_W, _GEAR360_H

    logger.info("ffprobe: detecting resolution (up to ~%ds; use --size WxH to skip)…", _FFPROBE_TIMEOUT)
    _flush_log()
    d = _ffprobe_dims(url)
    if d:
        return d
    logger.warning("ffprobe failed; using %dx%d. Pass --size WxH if the image is wrong.", _GEAR360_W, _GEAR360_H)
    return _GEAR360_W, _GEAR360_H


# ---------------------------------------------------------------------------
# Stream check (HEAD only — never opens the camera connection)
# ---------------------------------------------------------------------------

def check_stream_url(url: str, timeout: int = 5) -> tuple[bool, str]:
    """
    Use HEAD so the relay can answer without opening the upstream camera connection.
    Falls back to GET if the server rejects HEAD (405).
    """
    for method in ("HEAD", "GET"):
        try:
            req = urllib.request.Request(url, method=method)
            req.add_header("User-Agent", "Gear360Viewer/1.0")
            r = urllib.request.urlopen(req, timeout=timeout)
            code = r.getcode()
            r.close()
            if code in (200, 206):
                return True, "OK"
            return False, f"HTTP {code}"
        except urllib.error.HTTPError as e:
            if e.code == 405 and method == "HEAD":
                continue
            if e.code == 502:
                return False, "502 Bad Gateway (relay can't reach camera)"
            if e.code >= 500:
                return False, f"HTTP {e} (server error)"
            return False, f"HTTP {e.code}"
        except OSError as e:
            s = str(e)
            if "refused" in s.lower():
                return False, "Connection refused (relay not running?)"
            if "timed" in s.lower():
                return False, "Connection timed out"
            return False, s
    return False, "Stream check failed"


def _stream_hint(msg: str) -> str:
    m = msg.lower()
    if any(x in m for x in ("502", "500", "503", "bad gateway", "server error")):
        return " → enable live view on camera; Pi on camera WiFi; relay_stream.py running on Pi."
    if "refused" in m:
        return " → nothing listening on that address; is the relay up?"
    if "timed" in m:
        return " → check IP/firewall and that viewer.toml [stream] matches the relay."
    return ""


# ---------------------------------------------------------------------------
# Save directory / recording helpers
# ---------------------------------------------------------------------------

_WNAME      = "Gear360Viewer"
_TITLE_IDLE = "Gear360 Viewer  |  r: record   s: screenshot   q/ESC: quit"
_TITLE_REC  = "Gear360 Viewer  |  [REC]  r: stop   s: screenshot   q/ESC: quit"


def _saved_dir() -> str:
    """Return (and create) the saved_files directory next to this script."""
    d = os.path.join(_script_dir(), "saved_files")
    os.makedirs(d, exist_ok=True)
    return d


def _ts_name(ext: str) -> str:
    """Return a timestamped filename, e.g. gear360_20260427_183052.mp4."""
    return time.strftime(f"gear360_%Y%m%d_%H%M%S{ext}")


class _Recorder:
    """cv2.VideoWriter wrapper that writes to a temp file and renames on stop."""

    def __init__(self, width: int, height: int, fps: int) -> None:
        self._w = width
        self._h = height
        self._fps = fps if fps > 0 else 15
        self._writer: cv2.VideoWriter | None = None
        self._tmp_path = ""
        self.is_recording = False

    def start(self) -> bool:
        tmp = os.path.join(_saved_dir(), "gear360_recording_tmp.mp4")
        fourcc = cv2.VideoWriter_fourcc(*"mp4v")
        self._writer = cv2.VideoWriter(tmp, fourcc, float(self._fps), (self._w, self._h))
        if not self._writer.isOpened():
            logger.error("Could not create video file: %s", tmp)
            self._writer = None
            return False
        self._tmp_path = tmp
        self.is_recording = True
        logger.info("Recording started.")
        return True

    def write(self, frame) -> None:
        if self._writer and self.is_recording:
            self._writer.write(frame)

    def stop(self) -> str:
        """Flush, rename to a finish-timestamp filename, and return the final path."""
        if self._writer:
            self._writer.release()
            self._writer = None
        self.is_recording = False
        if not self._tmp_path or not os.path.isfile(self._tmp_path):
            return ""
        final = os.path.join(_saved_dir(), _ts_name(".mp4"))
        try:
            os.replace(self._tmp_path, final)
        except OSError as e:
            logger.error("Could not rename recording: %s", e)
            final = self._tmp_path
        self._tmp_path = ""
        logger.info("Recording saved: %s", final)
        return final

    def __del__(self) -> None:
        if self._writer:
            self._writer.release()


def _save_screenshot(frame) -> None:
    path = os.path.join(_saved_dir(), _ts_name(".png"))
    cv2.imwrite(path, frame)
    logger.info("Screenshot saved: %s", path)


# ---------------------------------------------------------------------------
# FFmpeg pipe backend (default for http)
# ---------------------------------------------------------------------------

def _put_frame(fq: queue.Queue, data: bytes | None) -> None:
    """Drop stale frames; keep only the newest."""
    while True:
        try:
            fq.get_nowait()
        except queue.Empty:
            break
    try:
        fq.put_nowait(data)
    except queue.Full:
        pass


def _read_frames(proc: subprocess.Popen, frame_size: int, fq: queue.Queue) -> None:
    buf = bytearray()
    try:
        while proc.stdout:
            need = frame_size - len(buf)
            chunk = proc.stdout.read(min(1024 * 1024, need)) if need > 0 else b""
            if not chunk:
                _put_frame(fq, None)
                return
            buf.extend(chunk)
            if len(buf) >= frame_size:
                _put_frame(fq, bytes(buf[:frame_size]))
                del buf[:frame_size]
            elif proc.poll() is not None:
                _put_frame(fq, None)
                return
    except Exception as e:
        logger.debug("frame reader: %s", e)
        _put_frame(fq, None)


# Printed when the live HTTP body ends mid-JPEG. The socket is already closed,
# so these are teardown noise; the viewer opens a new connection instead.
_FFMPEG_DROP_NOISE = (
    "low score of",
    "deprecated pixel format",
    "No JPEG data found",
    "Invalid data found when processing input",
    "Stream ends prematurely",
    "Error during demuxing",
    "HTTP error 502",
    "Server returned 5",
    "Error opening input",
)

_RECONNECT_BASE_S = 2.0
_RECONNECT_MAX_S = 8.0
_STALL_BEFORE_FIRST_S = 30.0
_STALL_AFTER_FRAMES_S = 8.0


def _drain_stderr(proc: subprocess.Popen) -> None:
    try:
        while proc.stderr:
            b = proc.stderr.read(4096)
            if not b:
                break
            for line in b.decode("utf-8", errors="replace").splitlines():
                t = line.strip()
                if not t:
                    continue
                if any(mark in t for mark in _FFMPEG_DROP_NOISE):
                    logger.debug("ffmpeg: %s", t)
                else:
                    logger.warning("ffmpeg: %s", t)
    except Exception:
        pass


def _stop_ffmpeg(proc: subprocess.Popen | None) -> None:
    if proc is None or proc.poll() is not None:
        return
    try:
        proc.kill()
    except Exception:
        pass
    try:
        proc.wait(timeout=3)
    except Exception:
        pass


def _start_ffmpeg(ffmpeg: str, url: str, frame_size: int, popen_kw: dict):
    """Start one decode process. A dropped Gear 360 connection cannot be resumed."""
    cmd = [
        ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "warning",
        "-analyzeduration", _FFMPEG_ANALYZE_US,
        "-probesize", _FFMPEG_PROBE_SIZE,
        "-i", url,
        "-an", "-c:v", "rawvideo", "-pix_fmt", "bgr24", "-f", "rawvideo", "pipe:1",
    ]
    logger.debug("ffmpeg: %s", " ".join(cmd))
    proc = subprocess.Popen(cmd, **popen_kw)
    fq: queue.Queue[bytes | None] = queue.Queue(maxsize=1)
    threading.Thread(target=_drain_stderr, args=(proc,), daemon=True).start()
    threading.Thread(target=_read_frames, args=(proc, frame_size, fq), daemon=True).start()
    return proc, fq


def _handle_viewer_key(key: int, session: _ViewSession, rec: _Recorder, last_view) -> bool:
    """Apply a window key. Return True when the user quits."""
    if key < 0:
        return False
    if key in (2424832, 65361):          # left
        session.nudge(-5.0, 0.0)
        return False
    if key in (2555904, 65363):          # right
        session.nudge(5.0, 0.0)
        return False
    if key in (2490368, 65362):          # up
        session.nudge(0.0, 5.0)
        return False
    if key in (2621440, 65364):          # down
        session.nudge(0.0, -5.0)
        return False
    low = key & 0xFF
    if low in (ord("q"), 27):
        logger.info("Exiting.")
        return True
    if low == ord("1"):
        session.set_mode("fisheye")
    elif low == ord("2"):
        session.set_mode("rectilinear")
    elif low == ord("3"):
        session.set_mode("360")
    elif low in (ord("t"), ord("T")):
        session.toggle_stitch()
    elif low in (ord("r"), ord("R")):
        if rec.is_recording:
            rec.stop()
        else:
            rec.start()
        session.set_recording(rec.is_recording)
    elif low in (ord("s"), ord("S")) and last_view is not None:
        _save_screenshot(last_view)
    return False


def _present(session: _ViewSession, source: np.ndarray, scale: float,
             rec: _Recorder, write: bool) -> np.ndarray:
    """Project, optionally record, draw the menu, and show the frame."""
    view = session.render(source)
    if write and rec.is_recording:
        rec.write(view if view.flags.writeable else view.copy())
    fh, fw = view.shape[:2]
    if scale != 1.0:
        disp = cv2.resize(view, (max(1, int(fw * scale)), max(1, int(fh * scale))))
    else:
        disp = view.copy()
    session.paint(disp)
    cv2.imshow(_WNAME, disp)
    session.needs_redraw = False
    return view


def _open_viewer_window(width: int, height: int, scale: float, session: _ViewSession) -> None:
    cv2.namedWindow(_WNAME, cv2.WINDOW_NORMAL)
    cv2.resizeWindow(_WNAME, max(1, int(width * scale)), max(1, int(height * scale)))
    cv2.setMouseCallback(_WNAME, session.on_mouse)
    session.update_title()


def run_ffmpeg_opencv(url: str, scale: float, cap_fps: int, width: int, height: int,
                      session: _ViewSession) -> int:
    ffmpeg = _find_ff("ffmpeg")
    if not ffmpeg:
        logger.error("ffmpeg not found. Install FFmpeg and add its bin/ to PATH.")
        return 1

    frame_size = width * height * 3
    logger.info("Frame size: %dx%d (BGR24)  view=%s  stitch=%s", width, height,
                session.mode, "on" if session.stitch_enabled else "off")

    _open_viewer_window(width, height, scale, session)

    popen_kw: dict = {"stdout": subprocess.PIPE, "stderr": subprocess.PIPE, "bufsize": 1024 * 1024}
    if sys.platform == "win32" and hasattr(subprocess, "CREATE_NO_WINDOW"):
        popen_kw["creationflags"] = subprocess.CREATE_NO_WINDOW

    proc, fq = _start_ffmpeg(ffmpeg, url, frame_size, popen_kw)
    logger.info("Decoding… first frame may take a few seconds.")

    rec = _Recorder(width, height, cap_fps)
    last_raw: np.ndarray | None = None
    last_view: np.ndarray | None = None
    fc, t0 = 0, time.time()
    last_log, last_fc, stall = t0, 0, 0.0
    drops = 0
    waiting_for_frame = True

    def _reconnect() -> bool:
        """Open a new connection. Return False when the user quits during the wait."""
        nonlocal proc, fq, stall, drops, last_view, waiting_for_frame
        _stop_ffmpeg(proc)
        drops += 1
        delay = min(_RECONNECT_MAX_S, _RECONNECT_BASE_S * drops)
        logger.warning("Stream dropped; reconnecting in %.0fs. Press q to quit.", delay)
        try:
            cv2.setWindowTitle(_WNAME, "Gear360 Viewer  |  reconnecting…  |  q: quit")
        except cv2.error:
            pass
        session._title = ""
        deadline = time.monotonic() + delay
        while time.monotonic() < deadline:
            if _handle_viewer_key(cv2.waitKeyEx(30), session, rec, last_view):
                return False
            if session.needs_redraw and last_raw is not None:
                last_view = _present(session, last_raw, scale, rec, write=False)
        proc, fq = _start_ffmpeg(ffmpeg, url, frame_size, popen_kw)
        stall = 0.0
        waiting_for_frame = True
        session._title = ""
        session.update_title()
        logger.info("Reconnecting to %s", url)
        return True

    try:
        while True:
            if _handle_viewer_key(cv2.waitKeyEx(1), session, rec, last_view):
                break

            try:
                raw = fq.get(timeout=0.04)
            except queue.Empty:
                if session.needs_redraw and last_raw is not None:
                    last_view = _present(session, last_raw, scale, rec, write=False)
                stall += 0.04
                limit = _STALL_BEFORE_FIRST_S if waiting_for_frame else _STALL_AFTER_FRAMES_S
                stalled = stall > limit
                if proc.poll() is not None or stalled:
                    if stalled and proc.poll() is None and waiting_for_frame:
                        logger.warning("No video yet — check relay and camera live-view mode.")
                    if not _reconnect():
                        break
                continue
            stall = 0.0
            if raw is None:
                if not _reconnect():
                    break
                continue
            drops = 0
            waiting_for_frame = False
            frame = np.frombuffer(raw, dtype=np.uint8).reshape((height, width, 3))
            last_raw = frame
            last_view = _present(session, frame, scale, rec, write=True)
            fc += 1
            if fc == 1:
                logger.info("First frame after %.1fs.", time.time() - t0)
            if cap_fps > 0:
                time.sleep(1.0 / cap_fps)
            now = time.time()
            if now - last_log >= 1.0:
                logger.info("FPS: ~%.1f | frames: %d", (fc - last_fc) / (now - last_log), fc)
                last_log, last_fc = now, fc
    except KeyboardInterrupt:
        logger.info("Interrupted.")
    finally:
        if rec.is_recording:
            rec.stop()
        try:
            proc.kill()
        except Exception:
            pass
        cv2.destroyAllWindows()
        elapsed = time.time() - t0
        if fc and elapsed:
            logger.info("frames=%d  avg_fps=%.1f", fc, fc / elapsed)
    return 0


# ---------------------------------------------------------------------------
# GStreamer helpers
# ---------------------------------------------------------------------------

_GST_Q_BUFFERS = 1
_GST_Q_TIME_MS = 0


def _gst_pipeline(url: str, buf: int, ms: int) -> str:
    ns = int(ms * 1e6) if ms else 0
    q = f"max-size-buffers={buf}" + (f" max-size-time={ns}" if ns else "")
    return f'uridecodebin uri="{url}" ! queue {q} ! videoconvert ! autovideosink sync=false'


def _find_gst_launch() -> str | None:
    exe = shutil.which("gst-launch-1.0") or shutil.which("gst-launch-1.0.exe")
    if exe:
        return exe
    if sys.platform != "win32":
        return None
    prog = os.environ.get("ProgramFiles", r"C:\Program Files")
    for base in (
        os.environ.get("GSTREAMER_1_0_ROOT_X86_64"),
        os.environ.get("GSTREAMER_1_0_ROOT_MSVC_X86_64"),
        os.path.join(prog, r"gstreamer\1.0\msvc_x86_64\bin"),
        r"C:\gstreamer\1.0\msvc_x86_64\bin",
    ):
        if base and os.path.isdir(base):
            c = os.path.join(base, "gst-launch-1.0.exe")
            if os.path.isfile(c):
                return c
    return None


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main() -> int:
    # ----------------------------------------------------------------
    # Priority: CLI flag > python/viewer.toml [viewer] > hardcoded default
    # All argparse defaults are None so we can detect "user did not pass this".
    # ----------------------------------------------------------------
    cfg = load_viewer_defaults()

    ap = argparse.ArgumentParser(
        description="Gear 360 live stream viewer.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            f"Config file: {_find_viewer_toml() or '(none — create python/viewer.toml)'}\n"
            f"Default URL:  {_DEFAULT_RELAY_URL}"
        ),
    )
    ap.add_argument("url", nargs="?", default=None,
                    help="Stream URL (default: [stream] in viewer.toml).")
    ap.add_argument("--resolution", default=None, choices=("high", "low"),
                    help="Camera live-view quality. The Gear 360 only serves these two "
                         "streams (livestream_high.avi / livestream_low.avi), not a custom WxH. "
                         f"(config: {cfg['resolution'] or 'url path'})")
    ap.add_argument("--size", default=None, metavar="WxH",
                    help=f"Decoder frame size e.g. 1280x640; skips ffprobe. Does not change "
                         f"what the camera sends. (config: {cfg['size'] or 'auto'})")
    ap.add_argument("--scale", type=float, default=None,
                    help=f"Window scale factor. (config: {cfg['scale']})")
    ap.add_argument("--fps", type=int, default=None,
                    help=f"Cap display rate Hz; 0 = uncapped. (config: {cfg['fps']})")
    ap.add_argument("--calibration", default=None,
                    help=f"Path to calibration.toml. (config: {cfg['calibration'] or 'auto'})")
    ap.add_argument("--log-file", default=None, metavar="FILE",
                    help=f"Append logs to file. (config: {cfg['log_file'] or 'stderr only'})")
    ap.add_argument("--log-level", default=None, choices=("DEBUG", "INFO", "WARNING", "ERROR"),
                    help=f"Verbosity. (config: {cfg['log_level']})")
    # Boolean flags: default=None lets us distinguish "not passed" from False.
    ap.add_argument("--ffplay", default=None, action="store_true",
                    help=f"External ffplay window. (config: {cfg['ffplay']})")
    ap.add_argument("--gstreamer", default=None, action="store_true",
                    help=f"GStreamer / ffplay on Windows. (config: {cfg['gstreamer']})")
    ap.add_argument("--ffmpeg-opencv", dest="ffmpeg_opencv", default=None, action="store_true",
                    help=f"Force FFmpeg pipe + OpenCV. (config: {cfg['ffmpeg_opencv']})")
    ap.add_argument("--opencv", dest="force_opencv", default=None, action="store_true",
                    help=f"Force OpenCV VideoCapture. (config: {cfg['opencv']})")
    ap.add_argument("--ignore-stream-check", default=None, action="store_true",
                    help=f"Skip pre-flight HEAD check. (config: {cfg['ignore_stream_check']})")
    ap.add_argument("--equirect", action="store_true", default=False,
                    help="Start in the draggable 360 view.")
    ap.add_argument("--fov", type=float, default=195.0,
                    help="Lens field of view in degrees for the projection and realtime stitch (default: 195).")

    a = ap.parse_args()

    # Apply config values for anything the user didn't pass on the CLI.
    def _cli(attr: str, cfg_key: str | None = None) -> None:
        k = cfg_key or attr
        if getattr(a, attr) is None:
            setattr(a, attr, cfg[k])

    _cli("size");            _cli("scale");           _cli("fps")
    _cli("resolution")
    _cli("calibration");     _cli("log_file");        _cli("log_level")
    _cli("ffplay");          _cli("gstreamer");        _cli("ffmpeg_opencv")
    _cli("force_opencv", "opencv");  _cli("ignore_stream_check")

    setup_logging((a.log_file or "").strip(), a.log_level or "INFO")

    # Resolve URL
    if not (a.url or "").strip():
        a.url = default_stream_url()
        vpath = _find_viewer_toml()
        logger.info("Stream URL from %s: %s", vpath or "default", a.url)
    if a.resolution:
        a.url = apply_stream_resolution(a.url, a.resolution)
        logger.info("Camera stream resolution: %s → %s", a.resolution, a.url)
    vcfg = load_viewer_toml_config()
    if vcfg:
        logger.info("viewer.toml: %s", vcfg["toml_path"])

    # Calibration
    camera_calib = None
    cal = _find_calibration_toml((a.calibration or "").strip())
    if cal:
        logger.info("Calibration: %s", cal)
        toml_params = load_calibration_toml(cal)
        md = toml_params.get("metadata") or {}
        if md.get("fov_deg") is not None or md.get("lens_fov_deg") is not None:
            logger.debug(
                "  metadata: fov_deg=%s",
                md.get("fov_deg", md.get("lens_fov_deg")),
            )
        for lens in ("lens1", "lens2"):
            lp = toml_params.get(lens, {})
            if lp:
                logger.debug(
                    "  %s: center=(%.4f, %.4f)  fov(raw)=%.4f  p1=%.4f p2=%.4f  "
                    "yaw=%.2f pitch=%.2f roll=%.2f (deg)",
                    lens,
                    lp.get("center_x", 0.5), lp.get("center_y", 0.5),
                    lp.get("fov", 1.0),
                    lp.get("p1", 0.0), lp.get("p2", 0.0),
                    lp.get("rotation_yaw", 0.0),
                    lp.get("rotation_pitch", 0.0),
                    lp.get("rotation_roll", 0.0),
                )
        camera_calib = _toml_to_camera_calibration(toml_params)
    else:
        logger.debug("No calibration file found (use --calibration <path> or set in viewer.toml)")

    lens_fov = float(a.fov) if float(a.fov) > 0 else 195.0
    if camera_calib is not None:
        try:
            cal_fov = min(float(camera_calib.lens1.fov), float(camera_calib.lens2.fov))
            if cal_fov > 0:
                lens_fov = cal_fov
        except (TypeError, ValueError, AttributeError):
            pass
    session = _ViewSession(lens_fov, camera_calib)
    if a.equirect:
        session.set_mode("360")
    logger.info(
        "Lens FOV %.1f°. Menu: Fisheye, Rectilinear, 360. Stitch starts off.",
        lens_fov,
    )

    # Stream check (HEAD — does not touch the camera connection)
    ok, msg = check_stream_url(a.url)
    if not ok:
        if a.ignore_stream_check:
            logger.warning("Stream check: %s — continuing (--ignore-stream-check).", msg)
        else:
            logger.error("Stream not ready: %s%s", msg, _stream_hint(msg))
            return 1

    # Backend selection
    if sum([a.ffplay, a.gstreamer, a.ffmpeg_opencv]) > 1:
        logger.error("Specify at most one of --ffplay / --gstreamer / --ffmpeg-opencv.")
        return 2

    use_pipe = a.ffmpeg_opencv or (
        not a.ffplay and not a.gstreamer and not a.force_opencv
        and a.url.startswith(("http://", "https://"))
        and _find_ff("ffmpeg") is not None
    )

    # --- FFmpeg pipe + OpenCV (default for http) ---
    if use_pipe:
        logger.info("Backend: FFmpeg pipe → OpenCV  (--opencv for direct capture, --ffplay for external).")
        w, h = resolve_stream_dims(a.url, a.size)
        return run_ffmpeg_opencv(a.url, a.scale, a.fps, w, h, session)

    # --- GStreamer ---
    if a.gstreamer:
        if sys.platform == "win32":
            fp = (vcfg.get("ffplay") if vcfg and os.path.isfile(vcfg.get("ffplay") or "") else None
                  ) or _find_ff("ffplay")
            if not fp:
                logger.error("--gstreamer on Windows uses ffplay; install FFmpeg or set external_player.ffplay.")
                return 1
            return subprocess.call([fp, "-hide_banner", "-fflags", "nobuffer",
                                    "-flags", "low_delay", "-framedrop", "-i", a.url])
        buf = (vcfg or {}).get("gst_queue_max_buffers", _GST_Q_BUFFERS)
        ms  = (vcfg or {}).get("gst_queue_max_time_ms",  _GST_Q_TIME_MS)
        gst = ((vcfg or {}).get("gst_launch") or "").strip() or _find_gst_launch() or ""
        if not gst:
            logger.error("gst-launch-1.0 not found; set PATH or external_player.gst_launch in viewer.toml.")
            return 1
        return subprocess.call([gst, "-v", "-e", _gst_pipeline(a.url, buf, ms)])

    # --- ffplay ---
    if a.ffplay:
        fp = None
        if vcfg and vcfg.get("ffplay") and os.path.isfile(vcfg["ffplay"]):
            fp = vcfg["ffplay"]
        fp = fp or _find_ff("ffplay")
        if not fp:
            logger.error("ffplay not found; install FFmpeg or set external_player.ffplay in viewer.toml.")
            return 1
        return subprocess.call([fp, "-hide_banner", "-fflags", "nobuffer",
                                "-flags", "low_delay", "-framedrop", "-i", a.url])

    # --- OpenCV direct capture ---
    logger.info("Backend: OpenCV VideoCapture. Press q or ESC to quit.")
    cap = cv2.VideoCapture(a.url, cv2.CAP_FFMPEG)
    cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
    if not cap.isOpened():
        logger.error("OpenCV could not open the stream. Try the default FFmpeg backend or --ffplay.")
        return 1
    w  = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))  or _GEAR360_W
    h  = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT)) or _GEAR360_H
    _open_viewer_window(w, h, a.scale, session)
    rec = _Recorder(w, h, a.fps)
    last_view = None
    fc, t0, lt, lfc = 0, time.time(), time.time(), 0
    try:
        while True:
            ok, fr = cap.read()
            if not ok or fr is None:
                break
            fc += 1
            last_view = _present(session, fr, a.scale, rec, write=True)
            if _handle_viewer_key(cv2.waitKeyEx(1), session, rec, last_view):
                break
            if a.fps > 0:
                time.sleep(1.0 / a.fps)
            now = time.time()
            if now - lt >= 1.0:
                logger.info("FPS: ~%.1f | frames: %d", (fc - lfc) / (now - lt), fc)
                lt, lfc = now, fc
    except KeyboardInterrupt:
        pass
    finally:
        if rec.is_recording:
            rec.stop()
        cap.release()
        cv2.destroyAllWindows()
        e = time.time() - t0
        if fc and e:
            logger.info("frames=%d  avg_fps=%.1f", fc, fc / e)
    return 0


if __name__ == "__main__":
    sys.exit(main())
