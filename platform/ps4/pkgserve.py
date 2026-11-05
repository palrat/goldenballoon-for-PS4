#!/usr/bin/env python3
"""Minimal HTTP file server with Range support.

The PS4 Remote Package Installer reads a package header with a partial request
before it starts downloading; Python's stock http.server ignores Range and the
console then fails with "Unable to load system file object for package".
"""
import os
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.abspath(sys.argv[1])
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8421
BIND = sys.argv[3] if len(sys.argv) > 3 else "0.0.0.0"


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("[srv] %s - %s\n" % (self.address_string(), fmt % args))

    def _path(self):
        p = os.path.normpath(os.path.join(ROOT, self.path.lstrip("/").split("?")[0]))
        return p if p.startswith(ROOT) else None

    def do_HEAD(self):
        self._send(head_only=True)

    def do_GET(self):
        self._send(head_only=False)

    def _send(self, head_only):
        path = self._path()
        if not path or not os.path.isfile(path):
            self.send_error(404)
            return
        size = os.path.getsize(path)
        start, end = 0, size - 1
        rng = self.headers.get("Range")
        partial = False
        if rng:
            m = re.match(r"bytes=(\d*)-(\d*)", rng.strip())
            if m:
                if m.group(1):
                    start = int(m.group(1))
                    if m.group(2):
                        end = min(int(m.group(2)), size - 1)
                elif m.group(2):
                    start = max(0, size - int(m.group(2)))
                partial = True
        if start > end or start >= size:
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{size}")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        length = end - start + 1
        self.send_response(206 if partial else 200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(length))
        if partial:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()
        if head_only:
            return
        with open(path, "rb") as f:
            f.seek(start)
            remaining = length
            while remaining > 0:
                chunk = f.read(min(262144, remaining))
                if not chunk:
                    break
                try:
                    self.wfile.write(chunk)
                except (BrokenPipeError, ConnectionResetError):
                    return
                remaining -= len(chunk)


if __name__ == "__main__":
    srv = ThreadingHTTPServer((BIND, PORT), Handler)
    sys.stderr.write(f"[srv] serving {ROOT} on {BIND}:{PORT}\n")
    sys.exit(srv.serve_forever())
