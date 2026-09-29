#!/usr/bin/env python3
"""Fake garnet_web device for UI work without flashing anything.

Serves web/ as separate files (edit + reload, no build step) and mimics the
device API from tools/fixtures/device.json: login/cookie/X-GW header, schema,
values, validation errors, live Info polling, WiFi scan/add/forget,
password change, backup/restore, and restart (the "device" is gone for 4 s).

  python3 tools/mock_server.py [--port 8080] [--portal]

Password: see "password" in the fixture (default "changeme").
Keep the API shapes in sync with src/gw_server.cpp and garnet_settings'
gs_json.cpp - this file is the contract's second implementation.
"""

import argparse
import copy
import http.server
import json
import pathlib
import re
import secrets
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
WEB = ROOT / "web"
FIXTURE = json.loads((ROOT / "tools" / "fixtures" / "device.json").read_text())

STATE = {
    "groups": copy.deepcopy(FIXTURE["groups"]),
    "password": FIXTURE["password"],
    "sessions": set(),
    "down_until": 0.0,
    "boot": time.time(),
    "scan_done_at": 0.0,
    "saved": list(FIXTURE["wifi"]["saved"]),
    "portal": False,
}
LOCK = threading.Lock()

IPV4 = re.compile(r"^(25[0-5]|2[0-4]\d|1\d\d|\d\d?)(\.(25[0-5]|2[0-4]\d|1\d\d|\d\d?)){3}$")
HOST = re.compile(r"^[A-Za-z0-9]([A-Za-z0-9-]{0,30}[A-Za-z0-9])?$")


def group(gid):
    return next((g for g in STATE["groups"] if g["id"] == gid), None)


def schema():
    out = []
    for g in STATE["groups"]:
        out.append({k: v for k, v in g.items() if k != "values"})
    return out


def validate(f, v):
    """Mirror of gsValidateInt / gsValidateString (+ the IPv4 / hostname
    validators the built-in tables attach)."""
    t = f["type"]
    if t == "toggle":
        return None if isinstance(v, bool) else "must be on or off"
    if t in ("number", "select"):
        if not isinstance(v, int) or isinstance(v, bool):
            return "must be a number"
        if t == "select":
            return None if 0 <= v < len(f["options"]) else "not one of the options"
        if v < f["min"] or v > f["max"]:
            return f"must be between {f['min']} and {f['max']}"
        if f.get("step", 1) > 1 and (v - f["min"]) % f["step"]:
            return f"must be in steps of {f['step']}"
        return None
    if t in ("text", "password"):
        if not isinstance(v, str):
            return "must be text"
        if len(v) > f.get("maxLen", 64):
            return f"at most {f.get('maxLen', 64)} characters"
        if f.get("ipv4") and v and not IPV4.match(v):
            return "not a valid IPv4 address"
        if f.get("hostname") and v and not HOST.match(v):
            return "letters, digits and - only"
        return None
    return None


def live_values(g):
    vals = g["values"]
    if g["id"] == "system":
        up = int(time.time() - STATE["boot"])
        vals["uptime"] = f"{up // 60}m {up % 60}s"
        vals["heap"] = f"{180 + (up % 7) * 0.3:.1f} KB free of 297.1 KB"
    if g["id"] == "wifi":
        vals["rssi"] = f"{-56 - (int(time.time()) % 5)} dBm (good)"
    return vals


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=str(WEB), **kw)

    def end_headers(self):
        # Static files: always revalidate, so an edit shows on plain reload.
        if not self.path.startswith("/api/"):
            self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    def log_message(self, fmt, *args):
        if "/api/live/" not in (args[0] if args else ""):
            super().log_message(fmt, *args)

    # ---- helpers -----------------------------------------------------------

    def send_json(self, obj, status=200, headers=None):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_svg(self, text):
        body = text.encode()
        self.send_response(200)
        self.send_header("Content-Type", "image/svg+xml")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def err(self, status, msg, field=None):
        o = {"ok": False, "error": msg}
        if field:
            o["field"] = field
        self.send_json(o, status)

    def authed(self):
        cookie = self.headers.get("Cookie", "")
        m = re.search(r"gw_s=([0-9a-f]{32})", cookie)
        return bool(m and m.group(1) in STATE["sessions"])

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        try:
            return json.loads(self.rfile.read(n) or b"{}")
        except ValueError:
            return None

    def down(self):
        if time.time() < STATE["down_until"]:
            # Simulate the device being unreachable mid-restart.
            self.close_connection = True
            try:
                self.connection.shutdown(2)
            except OSError:
                pass
            return True
        return False

    def reboot(self):
        STATE["down_until"] = time.time() + 4
        STATE["sessions"].clear()
        STATE["boot"] = time.time() + 4

    # ---- GET -----------------------------------------------------------------

    def do_GET(self):
        if self.down():
            return
        path = self.path.split("?")[0]
        if path == "/mock-cam.svg":
            return self.send_svg(MOCK_CAM)
        if not path.startswith("/api/"):
            return super().do_GET()
        with LOCK:
            if path == "/api/session":
                s = dict(FIXTURE["session"], auth=self.authed(), portal=STATE["portal"])
                return self.send_json(s)
            if not self.authed():
                return self.err(401, "login required")
            if path == "/api/schema":
                return self.send_json(schema())
            m = re.match(r"^/api/(group|live)/(\w+)$", path)
            if m:
                g = group(m.group(2))
                if not g:
                    return self.err(404, "no such group")
                vals = live_values(g)
                if m.group(1) == "live":
                    info = {f["key"] for f in g["fields"] if f["type"] == "info"}
                    return self.send_json({k: v for k, v in vals.items() if k in info})
                return self.send_json(vals)
            if path == "/api/wifi":
                scanning = time.time() < STATE["scan_done_at"]
                return self.send_json({
                    "mode": "portal" if STATE["portal"] else "wifi",
                    "portal": STATE["portal"],
                    "apSsid": "Relay Box-B2C0",
                    "connected": not STATE["portal"],
                    "ssid": "" if STATE["portal"] else "HomeNet",
                    "ip": "192.168.1.42",
                    "rssi": -58,
                    "saved": STATE["saved"],
                    "scan": {"state": "running" if scanning else "done",
                             "nets": [] if scanning else FIXTURE["wifi"]["nets"]},
                })
            if path == "/api/backup":
                groups = {}
                for g in STATE["groups"]:
                    vals = {f["key"]: g["values"][f["key"]] for f in g["fields"]
                            if f["type"] in ("toggle", "text", "number", "select")}
                    if vals:
                        groups[g["id"]] = vals
                return self.send_json({"garnet_settings": 1, "groups": groups}, headers={
                    "Content-Disposition": f'attachment; filename="{FIXTURE["session"]["host"]}-settings.json"'})
            return self.err(404, "no such endpoint")

    # ---- POST ----------------------------------------------------------------

    def do_POST(self):
        if self.down():
            return
        if self.headers.get("X-GW") != "1":
            return self.err(403, "missing X-GW header")
        path = self.path.split("?")[0]
        body = self.body()
        if body is None:
            return self.err(400, "invalid JSON")
        with LOCK:
            if path == "/api/login":
                if body.get("password") != STATE["password"]:
                    return self.err(401, "Wrong password", "password")
                tok = secrets.token_hex(16)
                STATE["sessions"].add(tok)
                return self.send_json({"ok": True}, headers={
                    "Set-Cookie": f"gw_s={tok}; Path=/; HttpOnly; SameSite=Strict"})
            if not self.authed():
                return self.err(401, "login required")
            if path == "/api/logout":
                STATE["sessions"].clear()
                return self.send_json({"ok": True})
            m = re.match(r"^/api/group/(\w+)$", path)
            if m:
                g = group(m.group(1))
                if not g:
                    return self.err(404, "no such group")
                changed = False
                pending = {}
                for f in g["fields"]:
                    if f["key"] not in body or f["type"] in ("info", "button"):
                        continue
                    v = body[f["key"]]
                    if f["type"] == "password" and (v == "" or isinstance(v, bool)):
                        continue
                    why = validate(f, v)
                    if why:
                        return self.err(422, why, f["key"])
                    pending[f["key"]] = v
                for k, v in pending.items():
                    if f_type(g, k) == "password":
                        v = bool(v)
                    if g["values"].get(k) != v:
                        g["values"][k] = v
                        changed = True
                reboot = bool(g.get("reboot")) and changed
                if reboot:
                    self.reboot()
                return self.send_json({"ok": True, "reboot": True} if reboot else {"ok": True})
            m = re.match(r"^/api/action/(\w+)/(\w+)$", path)
            if m:
                g = group(m.group(1))
                if g and m.group(2) == "pulsebtn":
                    g["values"]["last"] = time.strftime("%H:%M:%S") + " (pulse)"
                if g and m.group(2) == "snap":
                    g["values"]["lastsnap"] = f"/snap_{int(time.time()) % 100000}.jpg"
                return self.send_json({"ok": True})
            if path == "/api/password":
                if body.get("current") != STATE["password"]:
                    return self.err(422, "Current password is wrong")
                if len(body.get("next", "")) < 6:
                    return self.err(422, "Use at least 6 characters")
                STATE["password"] = body["next"]
                return self.send_json({"ok": True})
            if path == "/api/restore":
                if not isinstance(body.get("groups"), dict):
                    return self.err(422, "not a garnet_settings backup")
                n = 0
                for gid, vals in body["groups"].items():
                    g = group(gid)
                    if g:
                        g["values"].update(vals)
                        n += 1
                self.reboot()
                return self.send_json({"ok": True, "reboot": True, "restored": n})
            if path == "/api/wifi/scan":
                STATE["scan_done_at"] = time.time() + 2.5
                return self.send_json({"ok": True})
            if path == "/api/wifi/add":
                ssid, pw = body.get("ssid", ""), body.get("password", "")
                if not 1 <= len(ssid) <= 32:
                    return self.err(422, "SSID must be 1-32 characters", "ssid")
                if pw and not 8 <= len(pw) <= 63:
                    return self.err(422, "Password must be 8-63 characters", "password")
                if ssid not in STATE["saved"]:
                    STATE["saved"].append(ssid)
                STATE["portal"] = False
                self.reboot()
                return self.send_json({"ok": True, "reboot": True})
            if path == "/api/wifi/forget":
                if body.get("ssid") not in STATE["saved"]:
                    return self.err(404, "not saved")
                STATE["saved"].remove(body["ssid"])
                return self.send_json({"ok": True})
            return self.err(404, "no such endpoint")


# Stand-in for the device's MJPEG stream: an animated test card, so the
# img widget (sizing, pause-when-hidden) can be checked without a camera.
MOCK_CAM = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 640 480">
<rect width="640" height="480" fill="#223"/>
<g fill="none" stroke="#556" stroke-width="1">
<path d="M0 120h640M0 240h640M0 360h640M160 0v480M320 0v480M480 0v480"/></g>
<circle cx="320" cy="240" r="60" fill="#87ceeb"><animate attributeName="cx" values="120;520;120" dur="4s" repeatCount="indefinite"/></circle>
<text x="16" y="464" fill="#ccc" font-family="monospace" font-size="18">mock camera 640x480</text>
</svg>"""


def f_type(g, key):
    return next(f["type"] for f in g["fields"] if f["key"] == key)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--portal", action="store_true", help="start in setup-AP (captive portal) mode")
    args = ap.parse_args()
    STATE["portal"] = args.portal
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"garnet_web mock on http://localhost:{args.port}/  (password: {STATE['password']})")
    srv.serve_forever()


if __name__ == "__main__":
    main()
