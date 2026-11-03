#!/bin/bash
# Build and flash the firmware to the Clawdmeter board (Cmeter3.5).
# The board is found by its USB serial (daemon/clawd-port.sh) — NEVER
# /dev/ttyACM0 or pio's auto-detect: hidprj's ESP32 boards are attached here
# too, and flashing them would wipe their firmware. Absent board = abort.
#
# Usage: ./flash.sh            (CLAWD_SERIAL overrides the board serial)
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"

PORT="$(bash "$DIR/daemon/clawd-port.sh")" || {
    echo "Clawdmeter board (serial ${CLAWD_SERIAL:-1C:DB:D4:79:7F:3C}) not attached — refusing to flash." >&2
    exit 1
}
echo "Flashing via $PORT"

# The daemon writes to the port every poll; keep it off the port while flashing.
systemctl --user stop clawd-combined-serial.service 2>/dev/null || true
trap 'systemctl --user start clawd-combined-serial.service 2>/dev/null || true' EXIT

"$DIR/.piovenv/bin/pio" run -d "$DIR/firmware" -t upload --upload-port "$PORT"
