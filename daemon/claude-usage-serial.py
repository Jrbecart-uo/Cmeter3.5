#!/usr/bin/env python3
"""Claude Usage Tracker daemon — USB-serial transport (for WSL2 / no-Bluetooth hosts).

Polls the Anthropic API rate-limit headers (same call the BLE daemon makes) and
pushes the usage JSON to the ESP32 over USB serial instead of BLE. The firmware
accepts a `{...}` line on /dev/ttyACM0 exactly like the BLE RX payload.

Stdlib only (urllib + termios) — no pip deps. Reads the OAuth access token
fresh each poll from ~/.claude/.credentials.json (Claude Code keeps it
refreshed), so nothing sensitive is stored on the device.

Usage:
    python3 daemon/claude-usage-serial.py [/dev/ttyACM0]
    PORT=/dev/ttyACM0 POLL=60 python3 daemon/claude-usage-serial.py
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
POLL = int(os.environ.get("POLL", "60"))
CREDS = Path.home() / ".claude" / ".credentials.json"
API_URL = "https://api.anthropic.com/v1/messages"
BODY = json.dumps({
    "model": "claude-haiku-4-5-20251001",
    "max_tokens": 1,
    "messages": [{"role": "user", "content": "hi"}],
}).encode()


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def read_token():
    """Pull accessToken out of ~/.claude/.credentials.json (any nesting)."""
    txt = CREDS.read_text()
    try:
        def find(o):
            if isinstance(o, dict):
                for k, v in o.items():
                    if k == "accessToken" and isinstance(v, str):
                        return v
                    r = find(v)
                    if r:
                        return r
            elif isinstance(o, list):
                for v in o:
                    r = find(v)
                    if r:
                        return r
            return None
        tok = find(json.loads(txt))
        if tok:
            return tok
    except json.JSONDecodeError:
        pass
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', txt)
    return m.group(1) if m else None


def fetch_headers(token):
    req = urllib.request.Request(API_URL, data=BODY, method="POST", headers={
        "Authorization": f"Bearer {token}",
        "anthropic-version": "2023-06-01",
        "anthropic-beta": "oauth-2025-04-20",
        "Content-Type": "application/json",
        "User-Agent": "claude-code/2.1.5",
    })
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return dict(r.headers)
    except urllib.error.HTTPError as e:
        # Rate-limit headers are present even on 429/4xx responses.
        return dict(e.headers)


def build_payload(h):
    g = lambda k: h.get(k) or h.get(k.lower()) or h.get(k.title())
    now = time.time()

    def num(v, d=0.0):
        try:
            return float(v)
        except (TypeError, ValueError):
            return d

    u5 = num(g("anthropic-ratelimit-unified-5h-utilization"))
    r5 = num(g("anthropic-ratelimit-unified-5h-reset"))
    u7 = num(g("anthropic-ratelimit-unified-7d-utilization"))
    r7 = num(g("anthropic-ratelimit-unified-7d-reset"))
    st = g("anthropic-ratelimit-unified-5h-status") or "unknown"

    sr = max(0, round((r5 - now) / 60)) if r5 else 0
    wr = max(0, round((r7 - now) / 60)) if r7 else 0
    return json.dumps({
        "s": round(u5 * 100), "sr": sr,
        "w": round(u7 * 100), "wr": wr,
        "st": str(st).strip(), "ok": True,
    }, separators=(",", ":"))


def configure_port(path):
    # Put the tty in raw mode at 115200 (USB-CDC ignores baud, but harmless).
    os.system(f"stty -F {path} 115200 raw -echo -echoe -echok -echoctl -echonl 2>/dev/null")


def main():
    log(f"port={PORT} poll={POLL}s creds={CREDS}")
    while True:
        try:
            token = read_token()
            if not token:
                log("no accessToken in credentials.json — is Claude Code logged in?")
            else:
                payload = build_payload(fetch_headers(token))
                configure_port(PORT)
                # Serialize with the event-hook sender (clawd-event.sh) so
                # concurrent writes never interleave on the same line.
                with open(LOCK_PATH, "w") as lk:
                    fcntl.flock(lk, fcntl.LOCK_EX)
                    try:
                        with open(PORT, "wb", buffering=0) as fp:
                            fp.write(payload.encode() + b"\n")
                    finally:
                        fcntl.flock(lk, fcntl.LOCK_UN)
                log(f"sent: {payload}")
        except FileNotFoundError:
            log(f"{PORT} not found — is the device attached (usbipd)?")
        except Exception as e:  # noqa: BLE001 — daemon must never die
            log(f"error: {e!r}")
        time.sleep(POLL)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
