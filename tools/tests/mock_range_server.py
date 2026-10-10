#!/usr/bin/env python3
"""A host with a 130 MB file that takes byte ranges, for tools/tests/netstream_test.cpp. Byte i of the file is
(i * 31 + (i >> 16)) & 0xFF. Ranges that start at or after 40 MB answer HTTP 429 the first N times each (N from the
path: /file?fail=N), like a debrid host that is throttling. GET /__hits shows how many 429s were sent."""
import sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

SIZE = 130 * 1024 * 1024
seen, hits, lock = {}, [0], threading.Lock()

ROW = bytes((j * 31) & 0xFF for j in range(65536))
TABLES = [bytes((x + k) & 0xFF for x in range(256)) for k in range(256)]

def block(a, b):
    out = bytearray()
    for bi in range(a >> 16, (b >> 16) + 1):
        row = ROW.translate(TABLES[bi & 0xFF])
        lo = max(a, bi << 16) - (bi << 16)
        hi = min(b, ((bi + 1) << 16) - 1) - (bi << 16)
        out += row[lo:hi + 1]
    return bytes(out)

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_GET(self):
        u = urlparse(self.path)
        if u.path == "/__hits":
            body = str(hits[0]).encode()
            self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body); return
        fail = int(parse_qs(u.query).get("fail", ["0"])[0])
        rng = self.headers.get("Range", "")
        a, b = 0, SIZE - 1
        if rng.startswith("bytes="):
            lo, _, hi = rng[6:].partition("-")
            a = int(lo or 0); b = min(int(hi), SIZE - 1) if hi else SIZE - 1
        if a >= 40 * 1024 * 1024:
            with lock:
                seen[a] = seen.get(a, 0) + 1
                if seen[a] <= fail:
                    hits[0] += 1
                    self.send_response(429); self.send_header("Retry-After", "1"); self.send_header("Content-Length", "0"); self.end_headers(); return
        data = block(a, b)
        self.send_response(206 if rng else 200)
        self.send_header("Content-Type", "video/x-matroska"); self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Range", f"bytes {a}-{b}/{SIZE}"); self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        try: self.wfile.write(data)
        except Exception: pass

if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 18998), H).serve_forever()
