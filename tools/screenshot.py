#!/usr/bin/env python3
"""Render the DS18B20 tester web UI with mock data and save screenshots.

Extracts the INDEX_HTML block from src/main.cpp, serves it locally together
with a mock /data endpoint (the same JSON shape as buildJson() in the firmware)
and captures Chromium screenshots (desktop + mobile) into docs/img/.

No ESP32 hardware required — this is a pure front-end preview.
"""
import json
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parent.parent
MAIN = ROOT / "src" / "main.cpp"
OUT = ROOT / "docs" / "img"
OUT.mkdir(parents=True, exist_ok=True)

# --- extract the HTML from the firmware (the R"HTML( ... )HTML" block) ---
src = MAIN.read_text(encoding="utf-8")
m = re.search(r'R"HTML\((.*?)\)HTML"', src, re.DOTALL)
if not m:
    raise SystemExit("INDEX_HTML block not found in main.cpp")
HTML = m.group(1)

# --- realistic mock /data (shape matching buildJson()) ---
# "Warm glass" scenario: sensor #3 is the hottest (dipped into water).
MOCK = {
    "count": 8,
    "resolution": 12,
    "maxIndex": 3,
    "maxRom": "28FF641E0016035C",
    "maxTemp": 41.94,
    "sensors": [
        {"rom": "28A1B2C3D4000071", "esphome": "0x710000d4c3b2a128", "status": "ok",           "clone": False, "tempC": 23.31},
        {"rom": "28FF12AB3400006E", "esphome": "0x6e000034ab12ff28", "status": "ok",           "clone": False, "tempC": 23.06},
        {"rom": "2855AA1100008842", "esphome": "0x4288000011aa5528", "status": "ok",           "clone": True,  "tempC": 22.88},
        {"rom": "28FF641E0016035C", "esphome": "0x5c0316001e64ff28", "status": "ok",           "clone": False, "tempC": 41.94},
        {"rom": "28C9D8E7F6000015", "esphome": "0x150000f6e7d8c928", "status": "ok",           "clone": False, "tempC": 22.94},
        {"rom": "2877665544330021", "esphome": "0x2100334455667728", "status": "reset85",     "clone": False, "tempC": 85.00},
        {"rom": "28DEAD00BEEF0099", "esphome": "0x9900efbe00adde28", "status": "disconnected", "clone": False, "tempC": None},
        {"rom": "28ABCDEF12340055", "esphome": "0x55003412efcdab28", "status": "noread",      "clone": True,  "tempC": None},
    ],
}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path == "/" or self.path.startswith("/index"):
            body = HTML.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path.startswith("/data"):
            body = json.dumps(MOCK).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.end_headers()


def main():
    srv = HTTPServer(("127.0.0.1", 8765), Handler)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    try:
        with sync_playwright() as p:
            browser = p.chromium.launch()

            # Desktop
            page = browser.new_page(viewport={"width": 1100, "height": 900},
                                    device_scale_factor=2)
            page.goto("http://127.0.0.1:8765/")
            page.wait_for_function("document.getElementById('conn').textContent==='connected'",
                                   timeout=5000)
            page.wait_for_timeout(400)
            page.screenshot(path=str(OUT / "web-desktop.png"))
            print("saved", OUT / "web-desktop.png")

            # Mobile
            mob = browser.new_page(viewport={"width": 390, "height": 844},
                                   device_scale_factor=2, is_mobile=True)
            mob.goto("http://127.0.0.1:8765/")
            mob.wait_for_function("document.getElementById('conn').textContent==='connected'",
                                  timeout=5000)
            mob.wait_for_timeout(400)
            mob.screenshot(path=str(OUT / "web-mobile.png"), full_page=True)
            print("saved", OUT / "web-mobile.png")

            browser.close()
    finally:
        srv.shutdown()


if __name__ == "__main__":
    main()
