#!/usr/bin/env python3
"""A pretend image host for the preview's "artcache" check (tools/tests/run_art_test.sh). Paths like metahub's:
/poster/small|medium/<id>/img, /background/medium|big/<id>/img, plus /flaky/img (503 twice, then a poster) and
/missing/img (404). GET /__stats: how often each path was asked and how many connections were opened."""
import json, os, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "art")
FILES = {
    "/poster/small/tt1/img": "poster_small_300.jpg", "/poster/medium/tt1/img": "poster_medium_500.jpg",
    "/poster/small/tt2/img": "poster_small_300.jpg", "/poster/medium/tt2/img": "poster_medium_500.jpg",
    "/background/medium/same/img": "bg_medium_A_1280.jpg", "/background/big/same/img": "bg_big_A_2560.jpg",
    "/background/medium/diff/img": "bg_medium_A_1280.jpg", "/background/big/diff/img": "bg_big_B_3840.jpg",
    "/background/medium/wide/img": "bg_medium_C_1920.jpg", "/background/big/wide/img": "bg_big_B_3840.jpg",
}
hits, conns, flaky, lock = {}, set(), [0], threading.Lock()

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"  # keep-alive
    def log_message(self, *a): pass
    def send(self, code, body=b"", ctype="image/jpeg"):
        self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(body)))
        self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        with lock:
            hits[self.path] = hits.get(self.path, 0) + 1
            if self.path != "/__stats":
                conns.add(self.client_address[1])  # the app's connections (not the check's own)
        if self.path == "/__stats":
            return self.send(200, json.dumps({"hits": hits, "connections": len(conns)}).encode(), "application/json")
        if self.path == "/flaky/img":
            with lock:
                flaky[0] += 1; n = flaky[0]
            if n <= 2: return self.send(503, b"busy", "text/plain")
            return self.send(200, open(os.path.join(DIR, "poster_medium_500.jpg"), "rb").read())
        f = FILES.get(self.path)
        if not f: return self.send(404, b"no", "text/plain")
        self.send(200, open(os.path.join(DIR, f), "rb").read())

if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 18996), H).serve_forever()
