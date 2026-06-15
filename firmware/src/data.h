#pragma once
#include <Arduino.h>

struct UsageData {
    float session_pct;       // 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until session resets
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets
    char status[16];         // "allowed" or "limited"
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
};

// Status-board payload. The daemon does ALL the formatting and sends FLAT
// scalars (exactly like the usage payload), so the firmware only extracts
// strings — no array/loop, which is what hung the render:
//   {"sb":1,"sum":"16 / 18 up","dn":"Down: DB-prod DB-pp","red":1}
struct StatusData {
    char sum[48];    // headline, e.g. "16 / 18 up"
    char down[256];  // "Down: ..." or "all systems OK"
    bool red;        // true if anything is down (color the headline red)
    bool valid;
};
