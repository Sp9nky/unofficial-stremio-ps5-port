#!/usr/bin/env python3
"""A pretend Real-Debrid and Torbox for tools/tests/debrid_test.cpp: same endpoints and answers as the real APIs for
the calls src/debrid.cpp makes, plus GET /__log (every call so far) and POST /__reset."""
import json, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

KEY_RD, KEY_TB = "rd-test-key", "tb-test-key"
LOG, STATE = [], {}
CACHED_TB = {"aaaa" * 10: "Show.S01.1080p", "cccc" * 10: "Movie.2024.2160p"}
lock = threading.Lock()

def reset():
    STATE.clear(); STATE.update(rd_selected=False, tb_polls=0); LOG.clear()

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, body=None):
        data = b"" if body is None else (body if isinstance(body, bytes) else json.dumps(body).encode())
        self.send_response(code); self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data))); self.end_headers(); self.wfile.write(data)
    def body(self):
        n = int(self.headers.get("Content-Length") or 0); return self.rfile.read(n).decode("utf8", "replace") if n else ""
    def handle_any(self, method):
        u = urlparse(self.path); q = parse_qs(u.query); b = self.body(); path = u.path
        if path == "/__log": return self.send(200, LOG)
        if path == "/__reset": reset(); return self.send(200, {})
        auth = self.headers.get("Authorization", "")
        with lock:
            LOG.append(f"{method} {path}" + (" [multipart]" if "multipart" in (self.headers.get("Content-Type") or "") else ""))
            if path.startswith("/rd/"): return self.rd(method, path[4:], auth, b)
            if path.startswith("/tb/"): return self.tb(method, path[4:], auth, q, b)
        self.send(404, {"error": "no"})
    # ---- Real-Debrid
    def rd(self, method, p, auth, b):
        if p == "user": return self.send(200 if auth == "Bearer " + KEY_RD else 401, {"id": 1})
        if auth != "Bearer " + KEY_RD: return self.send(401, {"error": "bad_token"})
        if p == "torrents/addMagnet":
            STATE["rd_selected"] = False; STATE["rd_magnet"] = b
            return self.send(201, {"id": "RD1"})
        if p == "torrents/info/RD1":
            files = [{"id": 1, "path": "/Movie.2024.2160p.mkv", "bytes": 9000},
                     {"id": 2, "path": "/Sample/sample.mkv", "bytes": 10},
                     {"id": 3, "path": "/readme.txt", "bytes": 5},
                     {"id": 4, "path": "/Show.S01E02.1080p.mkv", "bytes": 4000}]
            done = STATE["rd_selected"] and "deadbeef" not in STATE.get("rd_magnet", "")
            return self.send(200, {"id": "RD1", "files": files, "status": "downloaded" if done else "waiting_files_selection",
                                   "links": ["https://rd.example/link1"] if done else []})
        if p == "torrents/selectFiles/RD1":
            STATE["rd_selected"] = True; STATE["rd_files"] = b
            return self.send(204)
        if p == "unrestrict/link":
            return self.send(200, {"download": "https://cdn.example/d/movie.mkv", "filename": "Movie.2024.2160p.mkv", "filesize": 9000})
        if p == "torrents/delete/RD1": return self.send(204)
        self.send(404, {})
    # ---- Torbox
    def tb(self, method, p, auth, q, b):
        if p == "v1/api/user/me": return self.send(200 if auth == "Bearer " + KEY_TB else 401, {"success": True})
        if p == "v1/api/user/auth/device/start":
            return self.send(200, {"success": True, "data": {"device_code": "dev123", "code": "ABCD-1234",
                "verification_url": "https://torbox.app/link", "friendly_verification_url": "https://tor.box/link?code=ABCD-1234",
                "interval": 1, "expires_at": "2099-01-01T00:00:00Z"}})
        if p == "v1/api/user/auth/device/token":
            STATE["tb_polls"] += 1
            if STATE["tb_polls"] < 3: return self.send(400, {"success": False, "error": "PENDING", "detail": "Device code has not been used yet"})
            return self.send(200, {"success": True, "data": {"access_token": KEY_TB}})
        if auth != "Bearer " + KEY_TB: return self.send(401, {"success": False})
        if p == "v1/api/torrents/checkcached":
            hashes = json.loads(b).get("hashes", [])
            return self.send(200, {"success": True, "data": {h: {"name": CACHED_TB[h], "size": 1} for h in hashes if h in CACHED_TB}})
        if p == "v1/api/torrents/createtorrent":
            if "dead" in b and "dead" * 10 in b: return self.send(409, {"success": False, "error": "NOT_CACHED"})
            if "overloaded" in b: return self.send(503, {})
            if "add_only_if_cached" not in b: return self.send(400, {})
            return self.send(200, {"success": True, "data": {"torrent_id": 7}})
        if p == "v1/api/torrents/mylist":
            return self.send(200, {"success": True, "data": {"id": 7, "files": [
                {"id": 0, "name": "Show.S01.1080p/Show.S01E01.mkv", "short_name": "Show.S01E01.mkv", "size": 3000, "mimetype": "video/x-matroska"},
                {"id": 1, "name": "Show.S01.1080p/Show.S01E02.mkv", "short_name": "Show.S01E02.mkv", "size": 3500, "mimetype": "video/x-matroska"},
                {"id": 2, "name": "Show.S01.1080p/info.nfo", "short_name": "info.nfo", "size": 1, "mimetype": "text/plain"}]}})
        if p == "v1/api/torrents/requestdl":
            return self.send(200, {"success": True, "data": f"https://tb.example/dl/{q.get('torrent_id',[''])[0]}/{q.get('file_id',[''])[0]}"})
        self.send(404, {})
    def do_GET(self): self.handle_any("GET")
    def do_POST(self): self.handle_any("POST")
    def do_DELETE(self): self.handle_any("DELETE")

if __name__ == "__main__":
    reset()
    ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 18999), H).serve_forever()
