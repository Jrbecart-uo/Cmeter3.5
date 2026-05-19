#pragma once

// Low-volume audio feedback via the on-board ES8311 codec + I2S.
// All calls are no-ops if the codec failed to init, so audio problems
// can never break the dashboard.

void sound_init(void);          // init ES8311 (low volume) + I2S TX
bool sound_ready(void);         // true if the codec initialised OK
void sound_chime(void);         // short pleasant blip — "usage data received"
void sound_alert(void);         // lower double-tone — status not OK / warning

// Distinct tone signature for a Claude-session event. `ev` is one of the
// game-sounds categories: "session-start", "task-acknowledge",
// "task-complete", "error", "permission". Unknown -> sound_chime().
void sound_event(const char* ev);
