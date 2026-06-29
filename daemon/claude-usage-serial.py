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
import shutil
import subprocess
import sys
import time
import urllib.request
import urllib.error
from pathlib import Path

LOCK_PATH = "/tmp/clawd-serial.lock"

PORT = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("PORT", "/dev/ttyACM0")
POLL = int(os.environ.get("POLL", "60"))

# --- usbipd self-heal (WSL2) -------------------------------------------------
# After a WSL restart the usbipd attach can silently die: usbipd still reports
# "Attached" but the USB-over-IP link resets (vhci_hcd urb->status -104) and
# /dev/ttyACM0 vanishes (or never enumerates). The daemon then writes into a
# void. When a *serial write* fails we run a detach->reattach cycle ourselves
# and wait out the reset burst until the port is stable again. Disable with
# HEAL=0. Only serial errors trigger this — network errors never do.
HEAL = os.environ.get("HEAL", "1").lower() not in ("0", "", "false", "no")
USBIPD_HWID = os.environ.get("USBIPD_HWID", "303a:1001")
# systemd --user PATH lacks the Windows dirs, so resolve usbipd.exe explicitly.
USBIPD_WSL_DISTRO = (os.environ.get("USBIPD_WSL_DISTRO")
                     or os.environ.get("WSL_DISTRO_NAME") or "Ubuntu-24.04")
HEAL_COOLDOWN = int(os.environ.get("HEAL_COOLDOWN", "45"))  # min s between heals
_last_heal = 0.0


def find_usbipd():
    cand = os.environ.get("USBIPD_EXE")
    if cand and Path(cand).exists():
        return cand
    default = "/mnt/c/Program Files/usbipd-win/usbipd.exe"
    if Path(default).exists():
        return default
    return shutil.which("usbipd.exe")


USBIPD = find_usbipd()
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


def send(payload):
    """Write one line to the serial port. Raises OSError if the port is gone
    or the USB link is dead — that's the signal the watchdog heals on."""
    configure_port(PORT)
    # Serialize with the event-hook sender (clawd-event.sh) so concurrent
    # writes never interleave on the same line.
    with open(LOCK_PATH, "w") as lk:
        fcntl.flock(lk, fcntl.LOCK_EX)
        try:
            with open(PORT, "wb", buffering=0) as fp:
                fp.write(payload.encode() + b"\n")
        finally:
            fcntl.flock(lk, fcntl.LOCK_UN)


def _run(cmd, timeout=30):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return r.returncode, ((r.stdout or "") + (r.stderr or "")).strip()
    except Exception as e:  # noqa: BLE001 — usbipd/interop hiccups must not kill us
        return -1, repr(e)


def wait_stable(path, appear=15, stable=4):
    """Wait for `path` to appear and then persist `stable`s (ride out the
    -104 reset burst that follows a fresh attach)."""
    deadline = time.time() + appear
    while time.time() < deadline:
        if os.path.exists(path):
            ok = True
            for _ in range(stable):
                time.sleep(1)
                if not os.path.exists(path):
                    ok = False
                    break
            if ok:
                return True
        time.sleep(1)
    return False


def reattach():
    """usbipd detach->reattach, then wait for a stable port. Cooldown-guarded
    so a genuinely-unplugged device doesn't get hammered every poll."""
    global _last_heal
    if not HEAL:
        return False
    if not USBIPD:
        log("heal: usbipd.exe not found (set USBIPD_EXE) — cannot self-heal")
        return False
    now = time.time()
    if now - _last_heal < HEAL_COOLDOWN:
        return False
    _last_heal = now
    log(f"heal: usbipd detach/reattach {USBIPD_HWID} -> {USBIPD_WSL_DISTRO}")
    _run([USBIPD, "detach", "--hardware-id", USBIPD_HWID])
    time.sleep(2)
    # One-shot attach (no --auto-attach: that flag blocks forever).
    rc, out = _run([USBIPD, "attach", "--wsl", USBIPD_WSL_DISTRO,
                    "--hardware-id", USBIPD_HWID])
    if rc != 0:
        log(f"heal: attach rc={rc} {out[:200]}")
    if wait_stable(PORT):
        log(f"heal: {PORT} back and stable")
        return True
    log(f"heal: {PORT} did not come back (device unplugged/powered off?)")
    return False


def main():
    log(f"port={PORT} poll={POLL}s creds={CREDS} "
        f"heal={'on' if (HEAL and USBIPD) else 'off'} usbipd={USBIPD}")
    while True:
        try:
            token = read_token()
            if not token:
                log("no accessToken in credentials.json — is Claude Code logged in?")
            else:
                # Network errors here fall through to the generic handler — they
                # must NOT trigger a usbipd reattach.
                payload = build_payload(fetch_headers(token))
                try:
                    send(payload)
                    log(f"sent: {payload}")
                except OSError as e:
                    # Port missing or USB link dead — heal and retry once.
                    log(f"{PORT} write failed ({e!r}) — attempting usbipd reattach")
                    if reattach():
                        try:
                            send(payload)
                            log(f"sent (post-heal): {payload}")
                        except OSError as e2:
                            log(f"{PORT} still failing post-heal: {e2!r}")
        except Exception as e:  # noqa: BLE001 — daemon must never die
            log(f"error: {e!r}")
        time.sleep(POLL)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
