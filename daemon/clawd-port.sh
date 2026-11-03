#!/bin/bash
# Print the Clawdmeter board's serial port, found by its USB serial number,
# or exit 1 if it isn't attached. NEVER fall back to /dev/ttyACM0: other
# ESP32/CH343 boards on this machine (hidprj) enumerate there too, and
# writing to them breaks their radio link / flashing.
#
# Usage: PORT=$(daemon/clawd-port.sh) || exit 1
#   CLAWD_SERIAL overrides the board serial (from `usbipd state`: the last
#   part of USB\VID_303A&PID_1001\<serial>).

SERIAL="${CLAWD_SERIAL:-1C:DB:D4:79:7F:3C}"
for p in /dev/serial/by-id/*"_${SERIAL}-if"*; do
    [ -e "$p" ] && { echo "$p"; exit 0; }
done
exit 1
