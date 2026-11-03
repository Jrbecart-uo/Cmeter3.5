# Clawdmeter must stop grabbing other ESP32 / USB-serial devices

Note from the agent working on `~/tmp/hidprj` (2026-10-22), written at the user's request. Please read it and apply the changes below.

## What is happening

The same laptop now also runs a second project: an ESP32-S3 acting as a **USB keyboard and mouse for Windows**, plus a second ESP32-S3 used as a **radio dongle**. Clawdmeter currently picks its device by generic names (`/dev/ttyACM0`, VID:PID `303a:1001`). Those names match hidprj's boards as well:

1. **Serial port.** `clawd-combined-serial.service` sets `PORT=/dev/ttyACM0`. When the Clawdmeter screen is not plugged in, hidprj's dongle becomes `/dev/ttyACM0`. The daemon opened it (PID 408, seen today) and:
   - stole the dongle's replies, so hidprj saw timeouts;
   - wrote its JSON lines into the radio link to the keyboard/mouse board. The CRC check dropped them, but writing to an unknown device is not safe.
2. **usbipd by hardware ID.** All of these match **every** ESP32-S3 in USB-JTAG mode (`303a:1001`), including hidprj's keyboard/mouse board:
   - `usbipd-autoattach.ps1` (bind and the logon `attach --auto-attach` task);
   - the daemon's heal: `usbipd detach --hardware-id 303a:1001`, then `attach --hardware-id 303a:1001`.

   This morning an auto-attach pulled the keyboard/mouse board into WSL and kept re-grabbing it while it was being flashed. That board **must stay on Windows**: if it is attached to WSL, Windows loses that keyboard and mouse. The user deliberately ran `usbipd unbind` on it, and a hardware-ID `bind` would undo that.

## Device inventory (from `usbipd state`, 2026-10-22)

| InstanceId (stable) | what | owner |
|---|---|---|
| `USB\VID_303A&PID_1001\1C:DB:D4:79:7F:3C` | **Clawdmeter screen** (Cmeter3.5 ESP32-S3) | **Clawdmeter**: the only device it may touch |
| `USB\VID_303A&PID_1001\84FCE6508CEC` | hidprj HID board (TinyUSB keyboard/mouse; busid 9-2 today, COM8) | hidprj: never bind/attach/detach/open |
| `USB\VID_1A86&PID_55D3\5735021536` | CH343 UART on the HID board (COM7) | hidprj |
| `USB\VID_303A&PID_1001\84:FC:E6:50:8B:E4` | hidprj radio dongle, USB-JTAG side (COM10) | hidprj |
| `USB\VID_1A86&PID_55D3\5735021596` | hidprj radio dongle, CH343 side; attached to WSL as `/dev/ttyACM0`, `/dev/serial/by-id/usb-1a86_USB_Single_Serial_5735021596-if00` | hidprj |

Busids and `/dev/ttyACMn` numbers change with the physical port and plug order. Only the serial number (the last part of the InstanceId) identifies a board.

## Changes requested

1. **Select the device by serial number, never by `ttyACM0`.**
   - Default `PORT` to the by-id path of the Clawdmeter board. For an ESP32-S3 USB-JTAG it should be `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_1C:DB:D4:79:7F:3C-if00`. Verify with `ls -l /dev/serial/by-id/` while the board is attached.
   - Or resolve it at startup: glob `/dev/serial/by-id/*1C:DB:D4:79:7F:3C*`. Also make the serial number configurable (e.g. `DEVICE_SERIAL=1C:DB:D4:79:7F:3C`).
   - Update `~/.config/systemd/user/clawd-combined-serial.service` (`Environment=PORT=...`), then run `systemctl --user daemon-reload` and `systemctl --user restart clawd-combined-serial`.
2. **If that device is absent, wait.** Do not fall back to any other tty, and never open or write to a port whose by-id name doesn't match.
3. **usbipd: target the exact instance, never `--hardware-id 303a:1001`.**
   - In the daemon heal: run `usbipd.exe state` (JSON) and find the entry whose `InstanceId` ends with `\1C:DB:D4:79:7F:3C`. Then use that entry's `BusId` for `usbipd detach --busid X` and `usbipd attach --wsl --busid X`. If it has no BusId (not plugged in), skip the heal.
   - In `usbipd-autoattach.ps1`: bind only that busid. Replace the `attach --auto-attach --hardware-id` logon task with one that resolves the busid from the InstanceId (the busid changes between ports), e.g. a small loop that polls `usbipd state` and attaches only that instance.
   - Then re-run the script so the scheduled task is replaced, and check that no `--hardware-id 303a:1001` watcher is left running.
4. **Be a good tty citizen (optional).** Before opening, check whether another process already has the port open (scan `/proc/*/fd`) and back off. hidprj now claims its port with `TIOCEXCL` and refuses to share it.

## How to verify

- Unplug the Clawdmeter screen. The daemon must log that it is waiting, and must not open `/dev/ttyACM0` (`fuser -v /dev/ttyACM0` shows no clawd process).
- Plug it back in. It reconnects through the by-id path.
- `usbipd list` must still show hidprj's `303a:1001` boards (9-2 and 10-2 today) as **Not shared**, untouched after a heal cycle.
