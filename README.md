# Cmeter3.5

A desk-side **Claude Code usage monitor + session-event notifier** on a 3.5″
ESP32-S3 touchscreen. It shows your live Anthropic rate-limit usage (5-hour and
weekly utilization + reset countdowns) and reacts to your Claude Code sessions
in real time with a coloured banner and a Warcraft-peasant voice line
(*"Ready to work!"*, *"Work work."*, *"Job's done!"*, …).

![Cmeter3.5 usage dashboard](screenshot.png)

> **This is a fork.** Cmeter3.5 is a hardware fork of
> **[Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter) by
> [@hermannbjorgvin](https://github.com/HermannBjorgvin)**, re-targeted from the
> original AMOLED board to the **Waveshare ESP32-S3-Touch-LCD-3.5 (non-"B")**
> and re-architected to feed the device over **USB serial** (no Bluetooth) so it
> can run from WSL2. Full credit to the upstream project for the concept and
> original firmware.

## Hardware

**[Waveshare ESP32-S3-Touch-LCD-3.5](https://www.waveshare.com/esp32-s3-touch-lcd-3.5.htm)**
(the non-"B" variant): **ST7796** 320×480 SPI LCD (run 480×320 landscape) +
**FocalTech FT6336** touch, AXP2101 PMU, ES8311 audio + speaker.

- Product page: <https://www.waveshare.com/esp32-s3-touch-lcd-3.5.htm>
- Wiki / specs: <https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-3.5>
- Schematic (PDF): <https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-3.5/ESP32-S3-Touch-LCD-3.5-Schematic.pdf>

> ⚠️ This is **not** the "3.5**B**" (AXS15231B QSPI). This board is ST7796 over
> plain SPI with FocalTech touch at I²C 0x38. See `DOCUMENTATION.md` for how to
> tell them apart and why it matters.

## Features

- Live Claude usage dashboard (5h + weekly %, reset countdowns), 480×320 landscape.
- Real-time **session events** via Claude Code hooks → coloured banner + the
  *actual* [game-sounds](https://github.com/citedy/claude-plugins) **warcraft**
  voice clips (embedded), mirroring its 5 event categories.
- Pixel-art "Clawd" splash animation (tap the screen to toggle).
- USB-serial transport — no Bluetooth, no buttons, USB-powered. Token never
  leaves the host.

## Quick start

Full step-by-step (Windows/WSL2 `usbipd` + auto-attach, build, flash, daemon,
hooks, troubleshooting, sound regeneration, history) is in **[`DOCUMENTATION.md`](DOCUMENTATION.md)**
(also rendered as `DOCUMENTATION.html`).

```bash
# build + flash (PlatformIO venv at repo root)
./.piovenv/bin/pio run -d firmware
./.piovenv/bin/pio run -d firmware -t upload --upload-port /dev/ttyACM0

# run the usage daemon (WSL2; device attached via usbipd).
# In practice this is wired as a systemd-user unit + a Windows
# Scheduled Task -- see DOCUMENTATION.md sections 3.2 / 3.4 for
# the hands-off (survives reboot/replug/reflash) setup.
nohup python3 daemon/claude-usage-serial.py /dev/ttyACM0 >/tmp/claude-usage.log 2>&1 &
```

Claude-event hooks are configured in `~/.claude/settings.json`
(SessionStart / UserPromptSubmit / Stop / PostToolUseFailure / Notification →
`daemon/clawd-event.sh`).

## Credits

- **Upstream concept & firmware:** [Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter)
  by [@hermannbjorgvin](https://github.com/HermannBjorgvin) — this project is a fork.
- **Clawd pixel-art animation:** [@amaanbuilds](https://claudepix.vercel.app).
- **Event sound effects:** the **game-sounds** Claude Code plugin (citedy) — warcraft pack.
- **ESP32-S3-Touch-LCD-3.5 port** (ST7796/FT6336, USB-serial, audio + event
  notifications): Jean-Roch Bécart.
