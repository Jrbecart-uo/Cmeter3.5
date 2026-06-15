#!/usr/bin/env python3
"""Status-board daemon - USB-serial transport for the Clawdmeter screen.

Reads the dt42 statusboard JSON (the same feed the web dashboard uses) and
pushes a compact status array to the ESP32 over USB serial, so the desk
screen shows a grid of green/red dots instead of Claude usage.

Run it wherever the device is plugged in:
  - on dt42 (recommended, always-on):  STATUS_URL defaults to localhost:3004
  - on a workstation:  STATUS_URL=http://10.136.128.10:3004/status.json (needs
    dt42 firewall port 3004 open)

Serial line sent (newline-delimited, same channel as the usage daemon):
  {"sb":[{"n":"fam","s":1},{"n":"co2","s":0},...],"ok":15,"down":2,"unk":1}
  s: 1=ok, 0=down, 2=unknown

Stdlib only. Usage:
  python3 daemon/statusboard-serial.py [/dev/ttyACM0]
  STATUS_URL=http://localhost:3004/status.json PORT=/dev/ttyACM0 POLL=30 \
      python3 daemon/statusboard-serial.py
  DRY=1 python3 daemon/statusboard-serial.py   # print payload, don't write serial
"""

import fcntl
import json
import os
import sys
import time
import urllib.request

LOCK_PATH = "/tmp/clawd-serial.lock"

PORT = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("PORT", "/dev/ttyACM0")
POLL = int(os.environ.get("POLL", "30"))
STATUS_URL = os.environ.get("STATUS_URL", "http://localhost:3004/status.json")
DRY = bool(os.environ.get("DRY"))

STATE_NUM = {"ok": 1, "down": 0, "unknown": 2}


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def fetch_status():
    req = urllib.request.Request(STATUS_URL, headers={"User-Agent": "statusboard-serial/1"})
    with urllib.request.urlopen(req, timeout=15) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


def build_payload(status):
    items = status.get("items", [])
    sb = [{"n": it.get("short", "?"), "s": STATE_NUM.get(it.get("state"), 2)} for it in items]
    ok = sum(1 for i in sb if i["s"] == 1)
    down = sum(1 for i in sb if i["s"] == 0)
    unk = sum(1 for i in sb if i["s"] == 2)
    return json.dumps({"sb": sb, "ok": ok, "down": down, "unk": unk},
                      separators=(",", ":"))


def configure_port(path):
    os.system(f"stty -F {path} 115200 raw -echo -echoe -echok -echoctl -echonl 2>/dev/null")


def write_serial(payload):
    configure_port(PORT)
    with open(LOCK_PATH, "w") as lk:
        fcntl.flock(lk, fcntl.LOCK_EX)
        try:
            with open(PORT, "wb", buffering=0) as fp:
                fp.write(payload.encode() + b"\n")
        finally:
            fcntl.flock(lk, fcntl.LOCK_UN)


def main():
    log(f"status_url={STATUS_URL} port={PORT} poll={POLL}s dry={DRY}")
    while True:
        try:
            payload = build_payload(fetch_status())
            if DRY:
                log(f"would send ({len(payload)}B): {payload}")
            else:
                write_serial(payload)
                log(f"sent ({len(payload)}B): {payload}")
        except FileNotFoundError:
            log(f"{PORT} not found - is the device attached?")
        except Exception as e:  # noqa: BLE001 - daemon must never die
            log(f"error: {e!r}")
        time.sleep(POLL)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
