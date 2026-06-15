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

// Status-board payload pushed by daemon/statusboard-serial.py:
//   {"sb":[{"n":"fam","s":1},...],"ok":15,"down":2,"unk":1}  (s: 1=ok 0=down 2=unknown)
#define SB_MAX 24

struct StatusItem {
    char name[16];
    uint8_t state;           // 1=ok, 0=down, 2=unknown
};

struct StatusData {
    StatusItem items[SB_MAX];
    int count;
    int ok, down, unk;       // summary counts
    bool valid;
};
