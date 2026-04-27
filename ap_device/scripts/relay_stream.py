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
import socket
import threading
import urllib.request
import urllib.error
import urllib.parse
import sys

# Defaults matching Gear 360 camera AP
DEFAULT_SOURCE_URL = "http://192.168.43.1:7679/livestream_high.avi"
DEFAULT_PORT = 7679
DEFAULT_BIND = "0.0.0.0"
CHUNK_SIZE = 64 * 1024  # 64 KB
STREAM_PATH = "/livestream_high.avi"


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
        default="",
        help="Only allow these client IPs (comma-separated); empty = allow all",
    )
    return p.parse_args()


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

    if method.upper() != "GET" or STREAM_PATH not in path:
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
        print("[%s] Camera HTTP error: %s %s" % (client_ip, e.code, e.reason))
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
        print("[%s] Upstream error: %s" % (client_ip, e))
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
            b"X-Relay-Version: 2\r\n"
        )  # so you can curl -I and confirm updated relay is running
        conn.sendall(b"\r\n")

        # Stream body
        while True:
            chunk = upstream.read(CHUNK_SIZE)
            if not chunk:
                break
            conn.sendall(chunk)
    except (BrokenPipeError, ConnectionResetError, socket.error) as e:
        print("[%s] Client disconnected: %s" % (client_ip, e))
    except Exception as e:
        print("[%s] Relay error: %s" % (client_ip, e))
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
    allow_list = [x.strip() for x in args.allow_ip.split(",") if x.strip()]

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        server.bind((args.bind, args.port))
    except OSError as e:
        print("Bind failed: %s" % e, file=sys.stderr)
        return 1
    server.listen(8)

    print("Gear 360 stream relay")
    print("  Source: %s" % args.source_url)
    print(
        "  Listen: http://%s:%s%s"
        % (args.bind if args.bind != "0.0.0.0" else "<all>:7679", args.port, STREAM_PATH)
    )
    if allow_list:
        print("  Allowed IPs: %s" % ", ".join(allow_list))
    print("On your PC, open: http://<PI_IP>:%s%s" % (args.port, STREAM_PATH))
    print("Press Ctrl+C to stop.\n")

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

