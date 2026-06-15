#pragma once
// ---------------------------------------------------------------------------
// Clawdmeter feature configuration
// ---------------------------------------------------------------------------
// Web Status screen (the dt42 status board) + Usage<->Status auto-rotation.
//
//   1 = enabled  : device shows BOTH the Claude usage screen and the "Web
//                  Status" dot grid, auto-rotating every ROTATE_MS, tap to
//                  advance. Requires the combined daemon (or the status daemon)
//                  to feed it status lines.
//   0 = disabled : original usage-only build. Status lines are ignored, the
//                  screen never rotates, and a tap toggles the splash as before.
//
// Flip this and re-flash; nothing else to change.
#define CLAWD_STATUS_SCREEN 1
