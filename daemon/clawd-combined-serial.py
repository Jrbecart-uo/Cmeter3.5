#!/usr/bin/env python3
"""Combined daemon: feeds BOTH Claude usage AND the dt42 status board to the
ESP32 over one USB-serial link, so the device can rotate between the two
screens. Replaces running claude-usage-serial.py and statusboard-serial.py
separately (they'd fight over the port).

Each cycle it sends two newline-delimited lines:
  {"s":..,"sr":..,"w":..,"wr":..,"st":..,"ok":true}        # usage
  {"sb":[{"n":"fam","s":1},...],"ok":16,"down":2,"unk":0}  # status

Stdlib only. Env:
  PORT=/dev/ttyACM0  POLL=30
  STATUS_URL=http://10.136.128.10:3004/status.json   (dt42 feed)
  DRY=1  -> print payloads, don't write serial
"""
import fcntl
import json
import os
import re
import sys
import time
import urllib.request
import urllib.error
from pathlib import Path

LOCK_PATH = "/tmp/clawd-serial.lock"
PORT = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("PORT", "/dev/ttyACM0")
POLL = int(os.environ.get("POLL", "30"))
STATUS_URL = os.environ.get("STATUS_URL", "http://10.136.128.10:3004/status.json")
DRY = bool(os.environ.get("DRY"))
CREDS = Path.home() / ".claude" / ".credentials.json"
API_URL = "https://api.anthropic.com/v1/messages"
BODY = json.dumps({"model": "claude-haiku-4-5-20251001", "max_tokens": 1,
                   "messages": [{"role": "user", "content": "hi"}]}).encode()
STATE_NUM = {"ok": 1, "down": 0, "unknown": 2}


def log(m):
    print(f"[{time.strftime('%H:%M:%S')}] {m}", flush=True)


# ---- usage ----
def read_token():
    try:
        txt = CREDS.read_text()
    except Exception:
        return None
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', txt)
    return m.group(1) if m else None


def usage_payload():
    token = read_token()
    if not token:
        return None
    req = urllib.request.Request(API_URL, data=BODY, method="POST", headers={
        "Authorization": f"Bearer {token}", "anthropic-version": "2023-06-01",
        "anthropic-beta": "oauth-2025-04-20", "Content-Type": "application/json",
        "User-Agent": "claude-code/2.1.5"})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            h = dict(r.headers)
    except urllib.error.HTTPError as e:
        h = dict(e.headers)
    g = lambda k: h.get(k) or h.get(k.lower()) or h.get(k.title())
    num = lambda v: float(v) if v not in (None, "") else 0.0
    now = time.time()
    try:
        u5, r5 = num(g("anthropic-ratelimit-unified-5h-utilization")), num(g("anthropic-ratelimit-unified-5h-reset"))
        u7, r7 = num(g("anthropic-ratelimit-unified-7d-utilization")), num(g("anthropic-ratelimit-unified-7d-reset"))
    except ValueError:
        return None
    st = g("anthropic-ratelimit-unified-5h-status") or "unknown"
    return json.dumps({"s": round(u5 * 100), "sr": max(0, round((r5 - now) / 60)) if r5 else 0,
                       "w": round(u7 * 100), "wr": max(0, round((r7 - now) / 60)) if r7 else 0,
                       "st": str(st).strip(), "ok": True}, separators=(",", ":"))


# ---- status ----
def status_payload():
    req = urllib.request.Request(STATUS_URL, headers={"User-Agent": "clawd-combined/1"})
    with urllib.request.urlopen(req, timeout=15) as r:
        st = json.loads(r.read().decode("utf-8", "replace"))
    items = st.get("items", [])
    total = len(items)
    ok = sum(1 for i in items if i.get("state") == "ok")
    down_names = [i.get("short", "?") for i in items if i.get("state") != "ok"]
    sum_s = f"{ok} / {total} up"
    down_s = ("Down: " + " ".join(down_names)) if down_names else "all systems OK"
    return json.dumps({"sb": 1, "sum": sum_s, "dn": down_s[:160],
                       "red": 1 if down_names else 0}, separators=(",", ":"))


def configure_port(path):
    os.system(f"stty -F {path} 115200 raw -echo -echoe -echok -echoctl -echonl -hupcl 2>/dev/null")


def send(line):
    if DRY:
        log(f"would send ({len(line)}B): {line[:80]}...")
        return
    configure_port(PORT)
    with open(LOCK_PATH, "w") as lk:
        fcntl.flock(lk, fcntl.LOCK_EX)
        try:
            with open(PORT, "wb", buffering=0) as fp:
                fp.write(line.encode() + b"\n")
        finally:
            fcntl.flock(lk, fcntl.LOCK_UN)


def main():
    log(f"port={PORT} poll={POLL}s status_url={STATUS_URL} dry={DRY}")
    while True:
        for label, fn in (("usage", usage_payload), ("status", status_payload)):
            try:
                p = fn()
                if p:
                    send(p)
                    log(f"{label} sent ({len(p)}B)")
                else:
                    log(f"{label} unavailable (skipped)")
            except FileNotFoundError:
                log(f"{PORT} not found - device attached?")
            except Exception as e:  # noqa: BLE001
                log(f"{label} error: {e!r}")
            time.sleep(0.4)  # small gap so the two lines don't coalesce
        time.sleep(POLL)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
