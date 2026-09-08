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
import socket
import subprocess
import sys
import threading
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


def find_herdr():
    cand = os.environ.get("HERDR_EXE")
    if cand and Path(cand).exists():
        return cand
    default = Path.home() / ".local" / "bin" / "herdr"
    if default.exists():
        return str(default)
    return shutil.which("herdr")


HERDR = find_herdr()
CREDS = Path.home() / ".claude" / ".credentials.json"
API_URL = "https://api.anthropic.com/v1/messages"
BODY = json.dumps({"model": "claude-haiku-4-5-20251001", "max_tokens": 1,
                   "messages": [{"role": "user", "content": "hi"}]}).encode()
STATE_NUM = {"ok": 1, "down": 0, "unknown": 2}


def log(m):
    print(f"[{time.strftime('%H:%M:%S')}] {m}", flush=True)


def fmt_clock(t=None):
    """'8:23am' — 12h, no leading zero (device has no RTC; host formats)."""
    return time.strftime("%I:%M%p", t or time.localtime()).lower().lstrip("0")


def clk_string():
    """Bottom-right corner string for the device: '8:23am · 04' (time + day)."""
    return f"{fmt_clock()} · {time.strftime('%d')}"


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
                       "st": str(st).strip(), "ok": True,
                       "clk": clk_string()}, separators=(",", ":"), ensure_ascii=False)


# ---- herd (herdr agents) ----
# Poll `herdr agent list` (local unix-socket CLI) and build a flat grid line,
# same shape as the status board:
#   {"hr":1,"g":"Clawdmeter=2;fam-k8s=1;...","sum":"1 working · 1 blocked",
#    "fl":"focus: fam-k8s","clk":"8:23am · 04"}
#   state: 0=idle 1=working 2=blocked 3=unknown; "*" prefix = focused pane.
HERD_STATE = {"idle": 0, "working": 1, "blocked": 2}
_prev_blocked = set()

# Live watch: the herdr socket pushes pane.agent_status_changed events, so the
# grid updates within ~2s instead of the 30s poll (which stays as a fallback
# refresh). The device is force-switched to the Herd screen only for statuses
# in HERD_SHOW — by default the ones that need eyes; working/idle flap on every
# tool call and would pin the screen.
HERDR_SOCK = os.environ.get("HERDR_SOCK",
                            str(Path.home() / ".config" / "herdr" / "herdr.sock"))
HERD_SHOW = set(os.environ.get("HERD_SHOW", "blocked,done").split(","))
HERD_DEBOUNCE = float(os.environ.get("HERD_DEBOUNCE", "2"))


def herd_payload(show=False):
    if not HERDR:
        return None
    rc, out = _run([HERDR, "agent", "list"], timeout=10)
    if rc != 0:
        raise RuntimeError(f"herdr agent list rc={rc}: {out[:120]}")
    agents = json.loads(out).get("result", {}).get("agents", [])
    if not agents:
        return None

    parts, counts = [], {"working": 0, "blocked": 0, "idle": 0}
    focused = None
    for a in agents:
        # 13 chars fits the device's 3-column herd grid (149px cells)
        label = os.path.basename(a.get("cwd", "") or "?")[:13]
        st = a.get("agent_status", "unknown")
        counts[st] = counts.get(st, 0) + 1
        if a.get("focused"):
            focused = label
            label = "*" + label[:12]
        parts.append(f"{label}={HERD_STATE.get(st, 3)}")

    # Edge-triggered "needs you" alert: a NEW blocked agent plays the existing
    # permission banner+sound on the device (routine polls stay silent).
    global _prev_blocked
    blocked_now = {a.get("pane_id") for a in agents
                   if a.get("agent_status") == "blocked"}
    new_blocked = blocked_now - _prev_blocked
    _prev_blocked = blocked_now
    if new_blocked:
        try:
            send('{"ev":"permission"}')
            log(f"herd: new blocked agent(s) {sorted(new_blocked)} — alerted")
        except OSError:
            pass  # main send path will heal the port; don't double-heal here

    sum_line = f"{counts['working']} working · {counts['blocked']} blocked"
    fl = f"focus: {focused}" if focused else ""
    obj = {"hr": 1, "g": ";".join(parts), "sum": sum_line,
           "fl": fl[:40], "clk": clk_string()}
    if show:
        obj["show"] = 1   # firmware switches to the Herd screen
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False)


def _herd_watch_once():
    """One subscribe-and-stream session. Returns True when exiting to refresh
    the pane subscription set (topology changed); raises on errors."""
    rc, out = _run([HERDR, "agent", "list"], timeout=10)
    if rc != 0:
        raise RuntimeError(f"agent list rc={rc}")
    agents = json.loads(out).get("result", {}).get("agents", [])
    subs = [{"type": "pane.agent_status_changed", "pane_id": a["pane_id"]}
            for a in agents if a.get("pane_id")]
    subs += [{"type": t} for t in
             ("pane.created", "pane.closed", "pane.agent_detected", "pane.focused")]

    sk = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sk.connect(HERDR_SOCK)
    try:
        sk.sendall((json.dumps({"id": "clawd", "method": "events.subscribe",
                                "params": {"subscriptions": subs}}) + "\n").encode())
        log(f"herd-watch: live ({len(agents)} panes)")
        sk.settimeout(300)
        last_push, buf = 0.0, b""
        while True:
            chunk = sk.recv(65536)
            if not chunk:
                raise RuntimeError("herdr socket closed")
            buf += chunk
            events = []
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                try:
                    m = json.loads(line)
                except ValueError:
                    continue
                if m.get("event"):
                    events.append(m)
            if not events:
                continue
            for e in events:
                d = e.get("data") or {}
                log(f"herd-watch: {e['event']}"
                    f" {d.get('pane_id', '')} {d.get('agent_status', '')}".rstrip())
            # Pane set changed -> re-list and re-subscribe with fresh pane ids.
            # NB: herdr emits status events dotted (pane.agent_status_changed)
            # but topology/focus events underscored (pane_agent_detected).
            if any(e["event"].replace("_", ".") in
                   ("pane.created", "pane.closed", "pane.agent.detected")
                   for e in events):
                return True
            show = any(e["event"] == "pane.agent_status_changed" and
                       (e.get("data") or {}).get("agent_status") in HERD_SHOW
                       for e in events)
            now = time.time()
            if not show and now - last_push < HERD_DEBOUNCE:
                continue
            last_push = now
            p = herd_payload(show=show)   # also fires the blocked banner+sound
            if p:
                try:
                    send(p)
                    log(f"herd event -> sent ({len(p)}B){' +show' if show else ''}")
                except OSError as e:
                    log(f"herd event send failed: {e!r}")  # 30s loop will heal
    finally:
        sk.close()


def herd_watcher():
    if not HERDR:
        return
    while True:
        resub = False
        try:
            resub = _herd_watch_once()
        except socket.timeout:
            resub = True   # idle stream; reconnect to pick up new panes
        except Exception as e:  # noqa: BLE001 — watcher must never die
            log(f"herd-watch: {e!r} — retrying")
        time.sleep(1 if resub else 10)


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
        # Append the time of the last poller check so an all-green board is
        # visibly fresh (a stale feed would otherwise look identical).
        gen, chk = st.get("generated", ""), None
        for pat in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M"):
            try:
                chk = fmt_clock(time.strptime(gen, pat))
                break
            except ValueError:
                pass
        lf = f"all systems OK @ {chk or fmt_clock()}"
    return json.dumps({"sb": 1, "g": g, "lf": lf[:54],
                       "clk": clk_string()}, separators=(",", ":"), ensure_ascii=False)


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
        f"heal={'on' if (HEAL and USBIPD) else 'off'} usbipd={USBIPD} "
        f"herdr={HERDR or 'off'} herd_show={','.join(sorted(HERD_SHOW))}")
    if HERDR and not DRY:
        threading.Thread(target=herd_watcher, daemon=True).start()
    while True:
        for label, fn in (("usage", usage_payload), ("status", status_payload),
                          ("herd", herd_payload)):
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
