# Project context

ESP32 firmware for a desk-side **Claude Code usage monitor + event notifier**, running on a
**Waveshare ESP32-S3-Touch-LCD-3.5** (the **non-"B"** variant: **ST7796** 320×480 SPI LCD +
**FocalTech FT6336** capacitive touch). A host-side daemon (in WSL2) polls the Anthropic API
and pushes usage + Claude-session events to the device over **USB serial**.

> ⚠️ History: the original upstream targeted a Waveshare AMOLED-2.16; this fork was first
> mis-ported to the "3.5**B**" (AXS15231B QSPI). The actual board is the **non-B 3.5**
> (ST7796 SPI + FT6336 @0x38). That mismatch caused a long black-screen hunt — see the
> memory files and DOCUMENTATION.md "History / gotchas". Don't re-litigate AXS15231B/QSPI.

Read this first, then `DOCUMENTATION.md` for the full install/troubleshooting guide.

## Hardware (critical, verified)

- **Display: ST7796**, standard 4-wire SPI. `Arduino_ESP32SPI(DC=3, CS=-1, SCK=5, MOSI=1, MISO=2)`
  + `Arduino_ST7796(bus, RST=-1, LCD_ROTATION, IPS=true, 320, 480)`. **HW rotation** → 480×320
  landscape (no Canvas). Backlight = GPIO **6** (active high, plain digitalWrite).
- **Touch: FT6336** @ I2C **0x38**, via SensorLib `TouchDrvFT6X36`.
- **TCA9554** I/O expander @ 0x20 — channel 1 pulses the LCD reset before `gfx->begin()`.
- **AXP2101** PMU @ 0x34 — `power_init()` enables the LCD power rails (factory sequence) +
  battery %. Intermittently fails to init on some boots (flaky shared I2C); battery-only impact.
- **ES8311** codec @ 0x18 + **NS4150B** amp (HW-enabled, no GPIO) + integrated speaker.
  I2S: MCLK=12, BCLK=13, LRCK=15, DOUT=16.
- **QMI8658** IMU @ 0x6B — init'd, unused (fixed landscape).
- Shared I2C bus: **SDA=8, SCL=7**.
- **No Bluetooth, no physical buttons** — both removed. Navigation is touch-only
  (tap toggles splash ↔ usage). USB-powered.

## Architecture

```text
main.cpp        — setup/loop; ST7796+FT6336 init; serial RX router
                  ({"s":..} usage  /  {"ev":..} events  /  "screenshot")
display_cfg.h   — pins, 480×320 landscape, extern objects
ui.{h,cpp}      — splash + usage screens (480×320 landscape) + event banner overlay
splash.{h,cpp}  — 20×20 pixel-art creature, 16× → 320×320 centred
sound.{h,cpp}   — ES8311/I2S; embedded warcraft voice clips per event + boot beep
sounds_warcraft.h — generated PCM (do NOT hand-edit; regen via ffmpeg+python, see DOCUMENTATION)
power.{h,cpp}   — AXP2101 rails + battery
imu.{h,cpp}     — QMI8658 (unused output)
data.h / usage_rate.* / icons.h / logo.h / font_*.c
lib/            — vendored: GFX_Library_for_Arduino 1.5.5 (Waveshare ST7796, SPI patched),
                  TCA9554, es8311
daemon/         — host side (run in WSL2):
  claude-usage-serial.py  usage poller → USB serial (stdlib only, flock-guarded)
  clawd-event.sh          Claude Code hook → {"ev":NAME} → serial (hook-safe, exit 0)
```

No BLE, no `ble.*`. Transport is USB serial only. Touch handled in `ui.cpp` via LVGL events.

## Build / flash

```bash
./.piovenv/bin/pio run -d firmware                                       # build
./.piovenv/bin/pio run -d firmware -t upload --upload-port /dev/ttyACM0  # flash
```

PlatformIO is not system-wide for this user — use `./.piovenv/bin/pio` (venv at repo root,
git-ignored). Env: `waveshare_touch_lcd_35`. Device = `/dev/ttyACM0` (USB-JTAG).

**WSL2:** `/dev/ttyACM0` only exists after, in a Windows **Admin** PowerShell:
`usbipd attach --wsl --busid 1-1` (re-run after every replug/reboot; `bind` persists once).

## Run (host daemon + hooks)

```bash
nohup python3 daemon/claude-usage-serial.py /dev/ttyACM0 >/tmp/claude-usage.log 2>&1 &
```
Hooks (usage events → device sound+banner) are in `~/.claude/settings.json` (SessionStart,
UserPromptSubmit, Stop, PostToolUseFailure, Notification → `daemon/clawd-event.sh`), mirroring
the `game-sounds` plugin's 5 categories. Hooks load at Claude Code session start.

## QA UI changes — don't ask the user

`screenshot` serial cmd dumps the LVGL framebuffer: `bash screenshot.sh out.png /dev/ttyACM0`
→ 480×320 PNG (auto-detects dims). Read it, verify, iterate. NOTE: this is the *framebuffer*,
not proof the physical panel is lit (that lesson cost hours — see history). Boots to
`SCREEN_USAGE` now (no button needed). Landscape layout constants are at the top of `ui.cpp`.

## Critical gotchas

1. **Wrong-board trap.** Symptoms of an AXS15231B/QSPI/SPI-mode-3/Canvas path = stale. This
   board is ST7796 **SPI** + FT6336. The `device/` Waveshare demo MUST be the **non-B 3.5**.
2. **Vendored GFX 1.5.5** (Waveshare's, has the correct ST7796 init). `Arduino_ESP32SPI.cpp`
   is patched for arduino-esp32 3.x (`spiFrequencyToClockDiv` 2-arg); QSPI/SPIDMA databus
   excluded via `lib/GFX_Library_for_Arduino/library.json` srcFilter. Don't swap to registry GFX.
3. **AXP2101 rails before display.** `power_init()` runs before TCA reset / `gfx->begin()`.
4. **Sound = events only.** Usage polls are silent (were beeping every 60s). Boot beep is an
   audio self-test. Embedded clips are loudnorm'd (raw mp3 was ~8% FS = inaudible).
5. **CRLF.** This machine's git mangled scripts (`screenshot.sh`, the ESP-IDF clone). Keep
   `.sh` LF; `screenshot.sh` ffmpeg size is dynamic from the device header.
6. **Repo hygiene.** `device/` (399MB, >100MB zip) is git-ignored — never commit it.
   `firmware/lib/` IS committed (required vendored deps). `.piovenv/` ignored.

## State (2026-05-19)

Fully working: ST7796 display, FT6336 touch, USB-serial live usage (reset times correct),
per-event colored banners + real game-sounds **warcraft** voice clips (one iconic clip/event,
embedded PCM), boot self-test beep. BLE + buttons removed. Open: AXP2101 init is intermittent
(battery-only); attribution credit line was on the deleted BT screen (relocate if wanted).
