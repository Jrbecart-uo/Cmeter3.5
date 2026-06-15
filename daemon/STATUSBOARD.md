# Status-board mode (side screen)

Repurposes the Clawdmeter screen to show the dt42 **status board** (a grid of
green/red/grey labels) instead of Claude usage. Two parts:

## 1. Daemon (deployable now, tested)
`daemon/statusboard-serial.py` reads the dt42 statusboard feed and pushes a
compact line to the device over USB serial:

```
{"sb":[{"n":"fam","s":1},{"n":"DB-prod","s":0},...],"ok":16,"down":2,"unk":0}
   s: 1=ok  0=down  2=unknown
```

Run it where the device is plugged in:
```bash
# dt42 (recommended, always-on) — reads its own local feed:
STATUS_URL=http://localhost:3004/status.json python3 daemon/statusboard-serial.py /dev/ttyACM0
# dry run (print payload, no device needed):
DRY=1 python3 daemon/statusboard-serial.py
```
Install as a user service:
```bash
cp daemon/statusboard-serial.service ~/.config/systemd/user/
systemctl --user daemon-reload && systemctl --user enable --now statusboard-serial.service
```
> Don't run this AND the usage daemon on the same device — they'd fight over
> `/dev/ttyACM0` (both take the `/tmp/clawd-serial.lock`, so writes won't
> corrupt, but the screen would flip between usage and status).

## 2. Firmware (compiles clean; runtime untested until flashed)
This branch adds a `SCREEN_STATUS` screen. The device auto-switches to it the
first time it receives an `{"sb":[...]}` line. Changes:
`data.h` (StatusData), `ui.h`/`ui.cpp` (the status screen, one recolor label =
the dot grid), `main.cpp` (parser + dispatcher branch, CMD_BUF_SIZE 256->1024).

Build & flash (per repo README):
```bash
./.piovenv/bin/pio run -d firmware
./.piovenv/bin/pio run -d firmware -t upload --upload-port /dev/ttyACM0
```
Verified to compile here (`pio run -d firmware` => [SUCCESS]); only the on-device
rendering is unverified. If flashing/rendering misbehaves, say so and I'll fix.
Names render colored (green=ok, red=down, grey=unknown); the small
screen shows names + summary counts, while the web/FAM dashboards carry the
last-failure timestamps.
