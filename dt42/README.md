# dt42 status poller

The poller that powers **Web Status mode** (see `../DOCUMENTATION.md` §8). It runs
on the host **`deepthought42` (dt42)**, not on the device — included here so the
whole system lives in one repo.

What it does:
- reads `targets.json` (the monitored list — websites/APIs/clusters/DB/machines),
- checks each target on an interval,
- serves `status.json` + a web dashboard on **`:3004`**,
- **pushes** the snapshot to the FAM web app(s) so the board is viewable in-app
  (`/admin/statusboard`).

The device gets this data via the combined serial daemon
(`../daemon/clawd-combined-serial.py`), which reads `:3004` and writes a flat
line to USB serial.

## Files
- `statusboard.py` — the poller + web server + FAM push (stdlib only).
- `index.html` — the `:3004` web dashboard.
- `statusboard.service` — systemd-user unit.
- `targets.example.json` — **sanitized** config. Copy to `targets.json` and put the
  real ingest token in `pushes[].token`.

> ⚠️ The live `~/statusboard/targets.json` contains the FAM **ingest token** and is
> **git-ignored** here. Never commit a `targets.json` with a real token — only the
> `.example.json` (placeholder) is tracked.

## Install (on dt42)
```bash
mkdir -p ~/statusboard
cp statusboard.py index.html ~/statusboard/
cp targets.example.json ~/statusboard/targets.json   # then edit: set the real token + targets
cp statusboard.service ~/.config/systemd/user/
systemctl --user daemon-reload && systemctl --user enable --now statusboard.service
```

Editing the monitored list, check types, and the field reference are documented in
`../DOCUMENTATION.md` §8.3.
