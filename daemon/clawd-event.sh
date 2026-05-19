#!/bin/bash
# Send a Claude-session event to the Clawdmeter device over USB serial.
# Used by Claude Code hooks. Mirrors the game-sounds plugin's event set:
#   session-start | task-acknowledge | task-complete | error | permission
#
# MUST never block or fail a Claude session: short flock timeout, always exit 0,
# silently no-op if the device isn't attached.
#
# Usage: clawd-event.sh <event-name>   (CLAWD_PORT overrides /dev/ttyACM0)

PORT="${CLAWD_PORT:-/dev/ttyACM0}"
LOCK="/tmp/clawd-serial.lock"
LOG="${CLAWD_LOG:-/tmp/clawd-events.log}"
EV="$1"

logline() { echo "[$(date '+%H:%M:%S')] $EV -> $1" >> "$LOG" 2>/dev/null; }

[ -z "$EV" ] && exit 0
[ -e "$PORT" ] || { logline "skip (no device)"; exit 0; }

{
    flock -w 2 9 || { logline "skip (lock busy)"; exit 0; }
    stty -F "$PORT" 115200 raw -echo 2>/dev/null
    printf '{"ev":"%s"}\n' "$EV" > "$PORT" 2>/dev/null && logline sent || logline "write failed"
} 9>"$LOCK" 2>/dev/null

exit 0
