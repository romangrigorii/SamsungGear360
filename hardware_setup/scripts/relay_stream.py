#!/usr/bin/env python3
"""
Gear 360 stream relay for Raspberry Pi (ap_device).

The Pi connects to the Gear 360's WiFi AP (camera is 192.168.43.1) and pulls
the MJPEG stream, then re-serves it on the Pi's LAN interface so your PC
(or any device on your network) can open the stream at http://PI_IP:PORT/...

Usage on Pi:
  1. Connect Pi's WiFi to the Gear 360's AP (camera in live view / streaming mode).
  2. Ensure Pi also has Ethernet (or second WiFi) on your main network.
  3. Run: python3 relay_stream.py [--port 7679] [--bind 0.0.0.0]
  4. On your PC: open http://<PI_IP>:7679/livestream_high.avi (or use the viewer).

Options:
  --source-url   Camera stream URL (default: http://192.168.43.1:7679/livestream_high.avi)
  --bind         Bind address (default: 0.0.0.0 = all interfaces)
  --port         Port to listen on (default: 7679)
  --allow-ip      Optional: only allow this client IP (comma-separated for multiple)
"""

import argparse
import logging
import os
import socket
import threading
import time
import urllib.request
import urllib.error
import urllib.parse
import sys

CHUNK_SIZE = 64 * 1024  # 64 KB
STREAM_PATH = "/livestream_high.avi"


def _relay_conf_paths():
    here = os.path.dirname(os.path.abspath(__file__))
    ap_root = os.path.abspath(os.path.join(here, ".."))
    return [
        os.path.join(ap_root, "config", "relay.conf"),
        os.path.join(ap_root, "relay.conf"),
    ]


def load_relay_conf():
    """
    Load optional KEY=VALUE settings from config/relay.conf (if present).
    Unknown keys are ignored. Lines may use CRLF.
    Supported keys:
      SOURCE_URL=http://192.168.43.1:7679/livestream_high.avi
      BIND=0.0.0.0
      PORT=7679
      ALLOW_IP=10.0.0.23
      LOG_FILE=/var/log/gear360_relay.log
      LOG_LEVEL=INFO
    """
    out = {}
    for path in _relay_conf_paths():
        if not os.path.isfile(path):
            continue
        try:
            with open(path, "r", encoding="utf-8") as f:
                for raw in f:
                    line = raw.strip().replace("\r", "")
                    if not line or line.startswith("#"):
                        continue
                    if "=" not in line:
                        continue
                    k, v = line.split("=", 1)
                    k = k.strip()
                    v = v.strip().strip('"').strip("'")
                    if k:
                        out[k.upper()] = v
        except OSError:
            continue
    return out


_RELAY_CONF = load_relay_conf()

# Defaults matching Gear 360 camera AP (can be overridden by config/relay.conf)
DEFAULT_SOURCE_URL = _RELAY_CONF.get("SOURCE_URL", "http://192.168.43.1:7679/livestream_high.avi")
DEFAULT_BIND = _RELAY_CONF.get("BIND", "0.0.0.0")
try:
    DEFAULT_PORT = int(str(_RELAY_CONF.get("PORT", "7679")).strip())
except Exception:
    DEFAULT_PORT = 7679
DEFAULT_ALLOW_IP = _RELAY_CONF.get("ALLOW_IP", "")
DEFAULT_LOG_FILE = _RELAY_CONF.get("LOG_FILE", "").strip()
DEFAULT_LOG_LEVEL = _RELAY_CONF.get("LOG_LEVEL", "INFO").strip().upper() or "INFO"


def parse_args():
    p = argparse.ArgumentParser(
        description="Relay Gear 360 MJPEG stream to your network (run on Raspberry Pi)."
    )
    p.add_argument(
        "--source-url",
        default=DEFAULT_SOURCE_URL,
        help="Camera stream URL (default: %s)" % DEFAULT_SOURCE_URL,
    )
    p.add_argument(
        "--bind",
        default=DEFAULT_BIND,
        help="Bind address (default: %s)" % DEFAULT_BIND,
    )
    p.add_argument(
        "--port",
        type=int,
        default=DEFAULT_PORT,
        help="Port to listen on (default: %s)" % DEFAULT_PORT,
    )
    p.add_argument(
        "--allow-ip",
        default=DEFAULT_ALLOW_IP,
        help="Only allow these client IPs (comma-separated); empty = allow all",
    )
    p.add_argument(
        "--log-file",
        default=DEFAULT_LOG_FILE,
        help="Append logs to this file (empty = stdout/stderr only). Can set LOG_FILE in relay.conf.",
    )
    p.add_argument(
        "--log-level",
        default=DEFAULT_LOG_LEVEL,
        choices=("DEBUG", "INFO", "WARNING", "ERROR"),
        help="Logging verbosity (default from relay.conf LOG_LEVEL or INFO)",
    )
    return p.parse_args()


def configure_logging(log_file, level_name):
    level = getattr(logging, level_name, logging.INFO)
    fmt = "%(asctime)s %(levelname)s [%(threadName)s] %(message)s"
    datefmt = "%Y-%m-%dT%H:%M:%SZ"

    root = logging.getLogger()
    root.handlers.clear()
    root.setLevel(level)

    class UtcFormatter(logging.Formatter):
        converter = time.gmtime

    formatter = UtcFormatter(fmt, datefmt)

    sh = logging.StreamHandler(sys.stderr)
    sh.setFormatter(formatter)
    root.addHandler(sh)

    if log_file:
        try:
            fh = logging.FileHandler(log_file, encoding="utf-8")
            fh.setFormatter(formatter)
            root.addHandler(fh)
        except OSError as e:
            print("Could not open --log-file %s: %s" % (log_file, e), file=sys.stderr)


def allowed_client(client_ip, allow_list):
    if not allow_list:
        return True
    return client_ip in allow_list


def handle_request(conn, source_url, allow_list):
    """Handle one HTTP request: proxy GET for the stream path to the camera."""
    client_addr = conn.getpeername()
    client_ip = client_addr[0] if isinstance(client_addr, tuple) else str(client_addr)

    if not allowed_client(client_ip, allow_list):
        try:
            conn.sendall(
                b"HTTP/1.1 403 Forbidden\r\n"
                b"Content-Type: text/plain\r\n"
                b"Connection: close\r\n\r\nForbidden\n"
            )
        except Exception:
            pass
        conn.close()
        return

    buf = b""
    while True:
        chunk = conn.recv(4096)
        if not chunk:
            conn.close()
            return
        buf += chunk
        if b"\r\n\r\n" in buf or b"\n\n" in buf:
            break
        if len(buf) > 8192:
            conn.close()
            return

    # Parse request line
    lines = buf.split(b"\r\n") if b"\r\n" in buf else buf.split(b"\n")
    request_line = lines[0].decode("utf-8", errors="replace").strip()
    parts = request_line.split()
    method = parts[0] if len(parts) >= 1 else ""
    path = parts[1] if len(parts) >= 2 else ""

    method_u = method.upper()
    if method_u not in ("GET", "HEAD") or STREAM_PATH not in path:
        try:
            conn.sendall(
                b"HTTP/1.1 404 Not Found\r\n"
                b"Content-Type: text/plain\r\n"
                b"Connection: close\r\n\r\nNot Found. Use GET "
                + STREAM_PATH.encode()
                + b"\n"
            )
        except Exception:
            pass
        conn.close()
        return

    # IMPORTANT:
    # HEAD must NOT open the upstream camera connection.
    # Many Gear360 setups behave like "single client" streams; urllib may also consume/trigger upstream reads for HEAD,
    # which can wedge the camera and break subsequent GET/ffplay sessions.
    if method_u == "HEAD":
        try:
            conn.sendall(
                b"HTTP/1.1 200 OK\r\n"
                b"Content-Type: video/x-msvideo\r\n"
                b"Cache-Control: no-cache\r\n"
                b"Connection: close\r\n"
                b"X-Relay-Version: 3\r\n"
                b"\r\n"
            )
        except Exception:
            pass
        conn.close()
        return

    # Open upstream stream to camera (short timeout so we fail fast and return 502 to client).
    # Set Host and User-Agent so the camera accepts the request (it can return 400 otherwise).
    try:
        parsed = urllib.parse.urlparse(source_url)
        host_header = parsed.netloc or "192.168.43.1:7679"
        req = urllib.request.Request(source_url)
        req.add_header("Host", host_header)
        req.add_header("User-Agent", "Samsung Gear 360 Manager/1.0")
        upstream = urllib.request.urlopen(req, timeout=4)
    except urllib.error.HTTPError as e:
        try:
            conn.sendall(
                b"HTTP/1.1 502 Bad Gateway\r\n"
                b"Content-Type: text/plain\r\n"
                b"Connection: close\r\n\r\nCamera returned HTTP "
                + str(e.code).encode()
                + b" "
                + (e.reason or "Error").encode()
                + b". Try: camera in live view, or check relay logs.\n"
            )
        except Exception:
            pass
        conn.close()
        logging.warning("Camera HTTP error from %s: %s %s", client_ip, e.code, e.reason)
        return
    except urllib.error.URLError as e:
        try:
            conn.sendall(
                b"HTTP/1.1 502 Bad Gateway\r\n"
                b"Content-Type: text/plain\r\n"
                b"Connection: close\r\n\r\nCannot reach camera: "
                + str(e).encode()
                + b"\n"
            )
        except Exception:
            pass
        conn.close()
        logging.warning("Upstream error from %s: %s", client_ip, e)
        return

    # Forward headers from camera to client (status + headers)
    try:
        # Status line (reason phrase for common codes)
        code = upstream.getcode()
        reason = {200: "OK", 206: "Partial Content", 302: "Found", 404: "Not Found"}.get(
            code, "OK"
        )
        conn.sendall(("HTTP/1.1 %d %s\r\n" % (code, reason)).encode())
        # Copy headers (exclude hop-by-hop and connection).
        # Drop Content-Length so clients read until close (camera sends 2^64-1; FFmpeg then says "stream ends prematurely").
        skip_headers = frozenset(
            ("transfer-encoding", "connection", "keep-alive", "proxy-", "content-length")
        )
        for name, value in upstream.headers.items():
            if name.lower().strip().rstrip("\r") in skip_headers:
                continue
            conn.sendall(("%s: %s\r\n" % (name, value)).encode())
        conn.sendall(b"Connection: close\r\n")
        conn.sendall(
            b"X-Relay-Version: 3\r\n"
        )  # so you can curl -I and confirm updated relay is running
        conn.sendall(b"\r\n")

        if method_u == "GET":
            # Stream body
            while True:
                chunk = upstream.read(CHUNK_SIZE)
                if not chunk:
                    break
                try:
                    conn.sendall(chunk)
                except (BrokenPipeError, ConnectionResetError, socket.error):
                    break
    except (BrokenPipeError, ConnectionResetError, socket.error) as e:
        logging.info("Client disconnected %s: %s", client_ip, e)
    except Exception as e:
        logging.exception("Relay error for %s: %s", client_ip, e)
    finally:
        try:
            upstream.close()
        except Exception:
            pass
        try:
            conn.close()
        except Exception:
            pass


def main():
    args = parse_args()
    configure_logging(args.log_file.strip(), args.log_level)
    allow_list = [x.strip() for x in args.allow_ip.split(",") if x.strip()]

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        server.bind((args.bind, args.port))
    except OSError as e:
        logging.error("Bind failed: %s", e)
        return 1
    server.listen(8)

    logging.info("Gear 360 stream relay starting")
    logging.info("Source: %s", args.source_url)
    listen_host = args.bind if args.bind != "0.0.0.0" else "<all>"
    logging.info("Listen: http://%s:%s%s", listen_host, args.port, STREAM_PATH)
    if allow_list:
        logging.info("Allowed IPs: %s", ", ".join(allow_list))
    logging.info("On your PC, open: http://<PI_IP>:%s%s", args.port, STREAM_PATH)
    logging.info("Press Ctrl+C to stop.")

    while True:
        try:
            conn, addr = server.accept()
        except KeyboardInterrupt:
            break
        t = threading.Thread(
            target=handle_request,
            args=(conn, args.source_url, allow_list),
            daemon=True,
        )
        t.start()

    server.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())

