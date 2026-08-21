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
import shutil
import subprocess
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

# --- usbipd self-heal (WSL2), same scheme as claude-usage-serial.py ----------
# The logon task's `usbipd attach --auto-attach` watcher can silently die; a
# later replug then never reaches WSL (/dev/ttyACM0 missing, or present but
# unwritable -> EACCES). Any OSError from the *serial write* — including
# FileNotFoundError and PermissionError — triggers a detach->reattach cycle.
# Network errors never do. Disable with HEAL=0.
HEAL = os.environ.get("HEAL", "1").lower() not in ("0", "", "false", "no")
USBIPD_HWID = os.environ.get("USBIPD_HWID", "303a:1001")
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
    return build_status_line(st)


def build_status_line(st):
    # Flat "name=state;..." grid string (no JSON array on-device) + a
    # pre-formatted "last fail" line built from the most recent failure.
    items = st.get("items", [])
    g = ";".join(f"{i.get('short', '?')}={STATE_NUM.get(i.get('state'), 2)}" for i in items)
    fails = [(i.get("short", "?"), i.get("last_failure")) for i in items
             if i.get("state") != "ok" and i.get("last_failure")]
    if fails:
        fails.sort(key=lambda x: x[1], reverse=True)
        lf = f"! last fail: {fails[0][0]}  {fails[0][1]}"
    else:
        lf = "all systems OK"
    return json.dumps({"sb": 1, "g": g, "lf": lf[:54]}, separators=(",", ":"))


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


def send_healing(line):
    """Serial write; any OSError (missing port, EACCES, dead USB link) heals
    the usbipd attach and retries once."""
    try:
        send(line)
        return True
    except OSError as e:
        log(f"{PORT} write failed ({e!r}) — attempting usbipd reattach")
        if reattach():
            send(line)  # let a second failure surface to the caller's log
            return True
        return False


def main():
    log(f"port={PORT} poll={POLL}s status_url={STATUS_URL} dry={DRY} "
        f"heal={'on' if (HEAL and USBIPD) else 'off'} usbipd={USBIPD}")
    while True:
        for label, fn in (("usage", usage_payload), ("status", status_payload)):
            try:
                # Payload build first: network errors land in the generic
                # handler below and must NOT trigger a usbipd reattach.
                p = fn()
                if p:
                    if send_healing(p):
                        log(f"{label} sent ({len(p)}B)")
                else:
                    log(f"{label} unavailable (skipped)")
            except Exception as e:  # noqa: BLE001
                log(f"{label} error: {e!r}")
            time.sleep(0.4)  # small gap so the two lines don't coalesce
        time.sleep(POLL)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
