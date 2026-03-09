#!/usr/bin/env python3
"""
Gear 360 stream player. Pass a stream URL and choose backend: OpenCV (default), --ffplay, or --gstreamer.
"""

import argparse
import os
import shutil
import subprocess
import sys
import time
import urllib.request
import urllib.error

import cv2

# GStreamer queue (video buffer) - edit these to change latency vs smoothness
GST_QUEUE_MAX_BUFFERS = 0  # max buffers in queue (1 = low latency)
GST_QUEUE_MAX_TIME_MS =1  # max buffer time in ms (0 = no limit)


def find_ffplay():
    """Return path to ffplay.exe, or None. On Windows, check WinGet install if not on PATH."""
    exe = shutil.which("ffplay") or shutil.which("ffplay.exe")
    if exe:
        return exe
    if sys.platform != "win32":
        return None
    # WinGet "FFmpeg (Essentials Build)" often installs here without adding bin to PATH
    local = os.environ.get("LOCALAPPDATA", "")
    if not local:
        return None
    packages = os.path.join(local, "Microsoft", "WinGet", "Packages")
    if not os.path.isdir(packages):
        return None
    for name in os.listdir(packages):
        if not name.startswith("Gyan.FFmpeg") or "Essentials" not in name:
            continue
        pkg = os.path.join(packages, name)
        if not os.path.isdir(pkg):
            continue
        for sub in os.listdir(pkg):
            if sub.startswith("ffmpeg-") and "essentials" in sub.lower():
                cand = os.path.join(pkg, sub, "bin", "ffplay.exe")
                if os.path.isfile(cand):
                    return cand
    return None


def gst_pipeline_string(url, buffer_buffers=1, buffer_time_ms=0):
    """Build GStreamer pipeline with configurable queue (buffer). Time in ns for queue max-size-time."""
    buffer_time_ns = int(buffer_time_ms * 1e6) if buffer_time_ms else 0
    queue_opts = f"max-size-buffers={buffer_buffers}"
    if buffer_time_ns > 0:
        queue_opts += f" max-size-time={buffer_time_ns}"
    return f'uridecodebin uri="{url}" ! queue {queue_opts} ! videoconvert ! autovideosink sync=false'


def run_gstreamer_in_process(url, buffer_buffers=1, buffer_time_ms=0):
    """Run GStreamer pipeline in-process via PyGObject (avoids Windows gst-launch cmd-line issues). Returns 0 on success."""
    try:
        import gi
        gi.require_version("Gst", "1.0")
        gi.require_version("GLib", "2.0")
        from gi.repository import Gst, GLib
    except (ImportError, ValueError):
        return None
    Gst.init(None)
    pipeline_str = gst_pipeline_string(url, buffer_buffers, buffer_time_ms)
    pipeline = Gst.parse_launch(pipeline_str)
    if not pipeline:
        return None
    loop = GLib.MainLoop()
    def on_bus_message(bus, message):
        if message.type in (Gst.MessageType.EOS, Gst.MessageType.ERROR):
            if message.type == Gst.MessageType.ERROR:
                err, _ = message.parse_error()
                print("GStreamer error:", err.message, file=sys.stderr)
            loop.quit()
    bus = pipeline.get_bus()
    bus.add_signal_watch()
    bus.connect("message::eos", on_bus_message)
    bus.connect("message::error", on_bus_message)
    pipeline.set_state(Gst.State.PLAYING)
    try:
        loop.run()
    except KeyboardInterrupt:
        pass
    pipeline.set_state(Gst.State.NULL)
    return 0


def find_gst_launch():
    """Return path to gst-launch-1.0, or None. On Windows, check common GStreamer install."""
    exe = shutil.which("gst-launch-1.0") or shutil.which("gst-launch-1.0.exe")
    if exe:
        return exe
    if sys.platform == "win32":
        # GStreamer MSVC/MinGW often install to Program Files or C:\gstreamer
        prog = os.environ.get("ProgramFiles", r"C:\Program Files")
        for base in [
            os.environ.get("GSTREAMER_1_0_ROOT_X86_64"),
            os.environ.get("GSTREAMER_1_0_ROOT_MSVC_X86_64"),
            os.path.join(prog, r"gstreamer\1.0\msvc_x86_64\bin"),
            os.path.join(prog, r"gstreamer\1.0\mingw_x86_64\bin"),
            r"C:\gstreamer\1.0\msvc_x86_64\bin",
            r"C:\gstreamer\1.0\mingw_x86_64\bin",
        ]:
            if not base or not os.path.isdir(base):
                continue
            cand = os.path.join(base, "gst-launch-1.0.exe")
            if os.path.isfile(cand):
                return cand
    return None


def check_stream_url(url, timeout=5):
    """Probe URL (headers only); return (ok, message). Don't read stream body."""
    try:
        req = urllib.request.Request(url)
        req.add_header("User-Agent", "Gear360Viewer/1.0")
        r = urllib.request.urlopen(req, timeout=timeout)
        code = r.getcode()
        r.close()
        if code in (200, 206):
            return True, "OK"
        return False, f"HTTP {code}"
    except urllib.error.HTTPError as e:
        if e.code == 502:
            return False, "502 Bad Gateway (relay can't reach camera)"
        return False, f"HTTP {e.code}"
    except OSError as e:
        if "Connection refused" in str(e) or "refused" in str(e).lower():
            return False, "Connection refused (relay not running?)"
        if "timed out" in str(e).lower() or "Timeout" in str(e):
            return False, "Connection timed out"
        return False, str(e)

def main():
    parser = argparse.ArgumentParser(description='Gear360 Video Stream Viewer')
    parser.add_argument('url', nargs='?', 
                       default='http://192.168.43.1:7679/livestream_high.avi',
                       help='Stream URL (default: http://192.168.43.1:7679/livestream_high.avi)')
    parser.add_argument('--fps', type=int, default=30,
                       help='Target FPS for display (default: 30)')
    parser.add_argument('--scale', type=float, default=1.0,
                       help='Scale factor for display window (default: 1.0)')
    parser.add_argument('--ffplay', action='store_true',
                       help='Use ffplay instead of OpenCV (often more reliable for MJPEG over HTTP)')
    parser.add_argument('--gstreamer', action='store_true',
                       help='Use GStreamer (gst-launch-1.0) to display the stream')
    
    args = parser.parse_args()
    
    if args.gstreamer:
        if sys.platform == "win32":
            # gst-launch-1.0 pipeline parsing is broken on Windows; use ffplay (same result)
            ffplay_exe = find_ffplay()
            if not ffplay_exe:
                print("ffplay not found. Install FFmpeg for --gstreamer on Windows.", file=sys.stderr)
                return 1
            return subprocess.call([
                ffplay_exe, "-hide_banner", "-fflags", "nobuffer",
                "-flags", "low_delay", "-framedrop", "-i", args.url
            ])
        result = run_gstreamer_in_process(args.url, GST_QUEUE_MAX_BUFFERS, GST_QUEUE_MAX_TIME_MS)
        if result is not None:
            return result
        gst = find_gst_launch()
        if not gst:
            print("gst-launch-1.0 not found. Install GStreamer and add its bin folder to PATH.", file=sys.stderr)
            return 1
        pipeline = gst_pipeline_string(args.url, GST_QUEUE_MAX_BUFFERS, GST_QUEUE_MAX_TIME_MS)
        return subprocess.call([gst, "-v", "-e", pipeline])
    
    if args.ffplay:
        ffplay_exe = find_ffplay()
        if not ffplay_exe:
            print("ffplay not found. Install FFmpeg and add its bin folder to PATH.", file=sys.stderr)
            return 1
        cmd = [
            ffplay_exe, "-hide_banner", "-fflags", "nobuffer",
            "-flags", "low_delay", "-framedrop", "-i", args.url
        ]
        return subprocess.call(cmd)
    
    # OpenCV path: optional quick check, then open stream
    ok, msg = check_stream_url(args.url)
    if not ok:
        print(f"Stream check: {msg}", file=sys.stderr)
    
    print("Press 'q' or ESC to quit")
    # Open video stream - use FFMPEG backend for MJPEG streams
    cap = cv2.VideoCapture(args.url, cv2.CAP_FFMPEG)
    cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
    
    if not cap.isOpened():
        cap = cv2.VideoCapture(args.url, cv2.CAP_FFMPEG)
        cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
    
    if not cap.isOpened():
        print(f"Could not open stream. Try: --ffplay or --gstreamer", file=sys.stderr)
        return 1
    
    # Get stream properties
    width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    fps = cap.get(cv2.CAP_PROP_FPS)
    
    print(f"Stream properties: {width}x{height} @ {fps:.2f} FPS")
    
    # Calculate display window size
    display_width = int(width * args.scale)
    display_height = int(height * args.scale)
    
    window_name = "Gear360 Viewer - Press 'q' or ESC to quit"
    cv2.namedWindow(window_name, cv2.WINDOW_NORMAL)
    cv2.resizeWindow(window_name, display_width, display_height)
    
    frame_count = 0
    start_time = time.time()
    last_fps_time = start_time
    last_fps_count = 0
    
    try:
        while True:
            ret, frame = cap.read()
            
            if not ret:
                print("Error: Failed to read frame")
                print("Stream may have ended or connection lost")
                break
            
            frame_count += 1
            
            # Calculate and display FPS every second
            current_time = time.time()
            if current_time - last_fps_time >= 1.0:
                fps_actual = (frame_count - last_fps_count) / (current_time - last_fps_time)
                print(f"FPS: {fps_actual:.2f} | Frames: {frame_count}")
                last_fps_time = current_time
                last_fps_count = frame_count
            
            # Resize frame if scale is not 1.0
            if args.scale != 1.0:
                frame = cv2.resize(frame, (display_width, display_height))
            
            # Display frame
            cv2.imshow(window_name, frame)
            
            # Check for exit keys
            key = cv2.waitKey(1) & 0xFF
            if key == ord('q') or key == 27:  # 'q' or ESC
                print("\nExiting...")
                break
            
            # Control frame rate if needed
            if args.fps > 0:
                time.sleep(1.0 / args.fps)
    
    except KeyboardInterrupt:
        print("\nInterrupted by user")
    except Exception as e:
        print(f"\nError: {e}")
        return 1
    finally:
        cap.release()
        cv2.destroyAllWindows()
        
        # Print statistics
        elapsed = time.time() - start_time
        if elapsed > 0:
            avg_fps = frame_count / elapsed
            print(f"\nStatistics:")
            print(f"  Total frames: {frame_count}")
            print(f"  Total time: {elapsed:.2f} seconds")
            print(f"  Average FPS: {avg_fps:.2f}")
    
    return 0

if __name__ == "__main__":
    sys.exit(main())
