#!/usr/bin/env python3
"""statusboard - tiny dependency-free uptime monitor.

Polls the targets in targets.json on an interval and serves:
  GET  /             -> the dashboard (index.html)
  GET  /status.json  -> current status of every target (CORS-open, no-cache)
  GET  /healthz      -> "ok"
  POST /heartbeat/<short> [ {"ok":true} ] -> record a push heartbeat
                        (used by the in-cluster MariaDB CronJob)

State (last_ok / last_failure per target) persists in state.json so a
restart doesn't lose the last-failure timestamps. targets.json is re-read
every cycle, so edits take effect on the next poll with no restart.
Stdlib only - runs on any Python 3.9+.
"""
import json
import os
import socket
import ssl
import subprocess
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
CFG_FILE = os.path.join(HERE, "targets.json")
STATE_FILE = os.path.join(HERE, "state.json")
INDEX_FILE = os.path.join(HERE, "index.html")

LOCK = threading.Lock()
STATUS = {"generated": None, "items": []}
HEARTBEATS = {}  # short -> {"ts": int, "ok": bool}


def now():
    return int(time.time())


def iso(ts):
    if not ts:
        return None
    return datetime.fromtimestamp(ts, timezone.utc).astimezone().strftime("%Y-%m-%d %H:%M")


def load_cfg():
    with open(CFG_FILE) as f:
        return json.load(f)


def load_state():
    try:
        with open(STATE_FILE) as f:
            return json.load(f)
    except Exception:
        return {}


def save_state(state):
    tmp = STATE_FILE + ".tmp"
    with open(tmp, "w") as f:
        json.dump(state, f)
    os.replace(tmp, STATE_FILE)


# ---- check primitives ----------------------------------------------------
def http_fetch(url, timeout, insecure=False, method="GET"):
    ctx = ssl.create_default_context()
    if insecure:
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
    req = urllib.request.Request(url, method=method, headers={"User-Agent": "statusboard/1"})
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=ctx) as r:
            return r.status, r.read(8192).decode("utf-8", "replace"), None
    except urllib.error.HTTPError as e:  # server answered with an error code
        try:
            body = e.read(8192).decode("utf-8", "replace")
        except Exception:
            body = ""
        return e.code, body, None
    except Exception as e:
        return None, "", f"{type(e).__name__}: {e}"


def check_http(t, timeout):
    code, body, err = http_fetch(t["url"], timeout, t.get("insecure", False), t.get("method", "GET"))
    if err:
        return False, err
    if "expect_body" in t:
        want = t["expect_body"].strip().lower()
        ok = want in (body or "").strip().lower()
        return ok, f"HTTP {code}, body{'=' if ok else '!='}'{t['expect_body']}'"
    if "ok_codes" in t:
        return code in t["ok_codes"], f"HTTP {code}"
    return code < t.get("ok_below", 500), f"HTTP {code}"


def check_tcp(t, timeout):
    try:
        with socket.create_connection((t["host"], int(t["port"])), timeout=timeout):
            return True, f"tcp {t['host']}:{t['port']} open"
    except Exception as e:
        return False, f"{type(e).__name__}: {e}"


def check_systemd_user(t, timeout):
    try:
        r = subprocess.run(
            ["systemctl", "--user", "is-active", t["unit"]],
            capture_output=True, text=True, timeout=timeout,
        )
        out = r.stdout.strip()
        return out == "active", f"systemd: {out or r.stderr.strip() or 'unknown'}"
    except Exception as e:
        return False, f"{type(e).__name__}: {e}"


def check_heartbeat(t):
    hb = HEARTBEATS.get(t["short"])
    if not hb:
        return None, "awaiting heartbeat (CronJob not deployed yet)"
    age = now() - hb["ts"]
    if age > t.get("max_age", 900):
        return False, f"stale: last heartbeat {age}s ago"
    return bool(hb.get("ok", True)), f"heartbeat {age}s ago"


def run_check(t, timeout):
    ty = t.get("type", "http")
    if ty in ("http", "https"):
        return check_http(t, timeout)
    if ty == "tcp":
        return check_tcp(t, timeout)
    if ty == "systemd_user":
        return check_systemd_user(t, timeout)
    if ty == "heartbeat":
        return check_heartbeat(t)
    return None, f"unknown check type '{ty}'"


def poll_loop():
    state = load_state()
    while True:
        try:
            cfg = load_cfg()
        except Exception as e:
            print(f"[statusboard] cannot read targets.json: {e}", flush=True)
            time.sleep(15)
            continue
        timeout = cfg.get("timeout", 8)
        items = []
        for t in cfg["targets"]:
            short = t["short"]
            try:
                ok, detail = run_check(t, timeout)
            except Exception as e:
                ok, detail = False, f"check crashed: {type(e).__name__}: {e}"
            st = state.get(short, {})
            ts = now()
            prev = st.get("state")
            if ok is True:
                stt = "ok"
                st["last_ok"] = ts
            elif ok is False:
                stt = "down"
                if prev != "down":  # record the moment it went down
                    st["last_failure"] = ts
            else:
                stt = "unknown"
            st["state"] = stt
            st["checked_at"] = ts
            st["detail"] = detail
            state[short] = st
            items.append({
                "name": t["name"],
                "short": short,
                "state": stt,
                "detail": detail,
                "last_ok": iso(st.get("last_ok")),
                "last_failure": iso(st.get("last_failure")),
                "checked_at": iso(ts),
            })
        with LOCK:
            STATUS["generated"] = iso(now())
            STATUS["items"] = items
            snapshot = json.dumps(STATUS).encode()
        try:
            save_state(state)
        except Exception as e:
            print(f"[statusboard] cannot save state: {e}", flush=True)
        # Push to FAM so the in-app dashboard works when remote (dt42 -> FAM:443
        # is open; the reverse is not). Failure here never blocks polling.
        pushes = cfg.get("pushes")
        if not pushes:
            one = cfg.get("push")
            pushes = [one] if one and one.get("url") else []
        for push in pushes:
            if not push.get("url"):
                continue
            try:
                req = urllib.request.Request(
                    push["url"], data=snapshot, method="POST",
                    headers={"Content-Type": "application/json",
                             "X-Statusboard-Token": push.get("token", "")},
                )
                urllib.request.urlopen(req, timeout=10).read()
            except Exception as e:
                print(f"[statusboard] push to {push['url']} failed: {e}", flush=True)
        time.sleep(cfg.get("poll_interval", 45))


# ---- http server ---------------------------------------------------------
class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="application/json"):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store, max-age=0")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path in ("/", "/index.html"):
            try:
                with open(INDEX_FILE, "rb") as f:
                    self._send(200, f.read(), "text/html; charset=utf-8")
            except Exception as e:
                self._send(500, str(e), "text/plain")
        elif path == "/status.json":
            with LOCK:
                data = json.dumps(STATUS).encode()
            self._send(200, data)
        elif path == "/healthz":
            self._send(200, b"ok", "text/plain")
        else:
            self._send(404, b'{"error":"not found"}')

    def do_POST(self):
        path = self.path.split("?")[0]
        if path.startswith("/heartbeat/"):
            short = path[len("/heartbeat/"):]
            ln = int(self.headers.get("Content-Length", 0) or 0)
            raw = self.rfile.read(ln) if ln else b"{}"
            try:
                payload = json.loads(raw or b"{}")
            except Exception:
                payload = {}
            HEARTBEATS[short] = {"ts": now(), "ok": bool(payload.get("ok", True))}
            self._send(200, b'{"recorded":true}')
        else:
            self._send(404, b'{"error":"not found"}')


def main():
    threading.Thread(target=poll_loop, daemon=True).start()
    try:
        port = int(os.environ.get("PORT") or load_cfg().get("port", 3004))
    except Exception:
        port = 3004
    srv = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    print(f"[statusboard] serving on 0.0.0.0:{port}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
