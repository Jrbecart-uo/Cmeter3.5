# Status-board mode (side screen) — WORKING

Shows the dt42 **status board** on the Clawdmeter, rotating with the Claude
usage screen. Verified on-device (screenshot-confirmed).

## How it works
The **daemon does all the formatting** and sends FLAT scalars (same style as the
usage payload — the firmware never parses an array, which is what used to hang
the render):

```
{"sb":1,"sum":"16 / 18 up","dn":"Down: DB-prod DB-pp","red":1}
```

Firmware: `SCREEN_STATUS` shows the headline (`sum`, red when `red`=1) + the
`dn` line. It auto-rotates Usage <-> Status every 30s once both have data, and
a **screen tap** advances immediately.

## Recommended: the combined daemon (both screens, one process)
`daemon/clawd-combined-serial.py` feeds BOTH Claude usage and the status board,
so the two daemons never fight over `/dev/ttyACM0`.

```bash
cp daemon/clawd-combined-serial.service ~/.config/systemd/user/
# disable the single-purpose ones first:
systemctl --user disable --now clawd-usage.service statusboard-serial.service 2>/dev/null
systemctl --user daemon-reload && systemctl --user enable --now clawd-combined-serial.service
```
Env in the unit: `STATUS_URL` (dt42 feed), `PORT=/dev/ttyACM0`, `POLL=30`.

`daemon/statusboard-serial.py` (status only) is still available if you want just
the board.

## Firmware build & flash
```bash
./.piovenv/bin/pio run -d firmware
./.piovenv/bin/pio run -d firmware -t upload --upload-port /dev/ttyACM0
```

## Debug aid
`./screenshot.sh out.png` dumps the live framebuffer over serial — invaluable
for verifying the screen without eyes on the device.

## Notes / gotchas
- Native USB-CDC: serial output does NOT flush before a firmware hang, so debug
  with `screenshot.sh` (or on-screen text), not Serial prints.
- ArduinoJson `x["k"] | false` is strict — an integer `1` is NOT a bool, so read
  flags with `.as<bool>()` (coerces 1/0). Same for `is<int>()` vs `is<bool>()`.
- The board groups by status: headline `N / total up` + a red `Down: ...` line.
  Full per-target detail + last-failure times live on the web/FAM dashboards.
