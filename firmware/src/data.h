#pragma once
#include <Arduino.h>

struct UsageData {
    float session_pct;       // 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until session resets
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets
    char status[16];         // "allowed" or "limited"
    char clk[20];            // host-formatted corner clock, e.g. "8:23am · 04"
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
};

// Status-board payload. The daemon sends a FLAT object whose "g" field is a
// delimited string (NOT a JSON array — a JSON array on-device hung the parse):
//   {"sb":1,"g":"fam=1;fam-pp=1;DB-prod=0;...","lf":"! last fail: ln2  2026-06-15 09:42"}
//   state: 1=ok 0=down 2=unknown.  The firmware splits "g" with strtok (plain C).
#define SB_MAX 24

struct StatusItem {
    char name[16];
    uint8_t state;           // 1=ok, 0=down, 2=unknown
};

struct StatusData {
    StatusItem items[SB_MAX];
    int count;
    char lastfail[56];       // pre-formatted bottom line
    char clk[20];            // host-formatted corner clock, e.g. "8:23am · 04"
    bool valid;
};

// Herd (herdr agents) payload — same flat shape as the status board:
//   {"hr":1,"g":"*fam-k8s=1;Clawdmeter=2;...","sum":"1 working · 1 blocked",
//    "fl":"focus: fam-k8s","clk":"8:23am · 04"}
//   state: 0=idle 1=working 2=blocked 3=unknown; "*" name prefix = focused.
struct HerdData {
    StatusItem items[SB_MAX];
    int count;
    char sum[40];            // pre-formatted "N working · M blocked"
    char focus[44];          // pre-formatted "focus: <label>" (may be empty)
    char clk[20];
    bool valid;
};
