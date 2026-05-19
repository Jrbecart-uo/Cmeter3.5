#include "sound.h"
#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <ESP_I2S.h>
#include "es8311.h"
#include "sounds_warcraft.h"   // real game-sounds (warcraft) clips, 22050 mono

// ---- I2S pins (ESP32-S3-Touch-LCD-3.5, from the board's 04_es8311 example) ----
#define I2S_MCLK   12
#define I2S_BCLK   13
#define I2S_LRCK   15
#define I2S_DOUT   16   // ESP -> codec DAC -> speaker
#define I2S_DIN    14

#define SND_RATE      22050              // shared by tones + embedded voice clips
#define SND_MCLK      (SND_RATE * 256)
#define SND_VOLUME    60    // 0..100 — audible but moderate (was 22 = inaudible)

static I2SClass i2s;
static bool ready = false;

static void play_tone(float freq, uint16_t ms, float amp = 0.6f);

void sound_init(void) {
    // ES8311 lives on the shared I2C bus (addr 0x18). Wire.begin(SDA,SCL)
    // has already run in setup(); the es8311 driver uses I2C_NUM_0 (same
    // port Arduino's Wire uses), matching the board's official example.
    es8311_handle_t es = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
    if (!es) {
        Serial.println("ES8311 create failed — sound disabled");
        return;
    }
    es8311_clock_config_t clk = {};
    clk.mclk_inverted    = false;
    clk.sclk_inverted    = false;
    clk.mclk_from_mclk_pin = true;
    clk.mclk_frequency   = SND_MCLK;
    clk.sample_frequency = SND_RATE;
    if (es8311_init(es, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) {
        Serial.println("ES8311 init failed — sound disabled");
        return;
    }
    es8311_voice_volume_set(es, SND_VOLUME, NULL);
    es8311_microphone_config(es, false);

    i2s.setPins(I2S_BCLK, I2S_LRCK, I2S_DOUT, I2S_DIN, I2S_MCLK);
    if (!i2s.begin(I2S_MODE_STD, SND_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                   I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
        Serial.println("I2S begin failed — sound disabled");
        return;
    }
    ready = true;
    Serial.println("ES8311 sound init OK");
    // Boot confirmation tone — if you hear this, audio works (speaker + amp OK).
    play_tone(880.0f, 90);
    play_tone(1175.0f, 130);
}

bool sound_ready(void) { return ready; }

// Play a tone for `ms` at `freq` Hz with a short attack/release envelope
// (avoids clicks). 16-bit stereo, amplitude kept modest.
static void play_tone(float freq, uint16_t ms, float amp) {
    if (!ready) return;
    const int total = (int)((uint32_t)SND_RATE * ms / 1000);
    const int fade  = SND_RATE / 200;  // ~5ms attack/release
    int16_t buf[256 * 2];
    int done = 0;
    while (done < total) {
        int n = total - done;
        if (n > 256) n = 256;
        for (int i = 0; i < n; i++) {
            int idx = done + i;
            float env = 1.0f;
            if (idx < fade)              env = (float)idx / fade;
            else if (idx > total - fade) env = (float)(total - idx) / fade;
            float s = sinf(2.0f * (float)M_PI * freq * idx / SND_RATE);
            int16_t v = (int16_t)(s * env * amp * 32767.0f);
            buf[i * 2] = v;       // L
            buf[i * 2 + 1] = v;   // R
        }
        i2s.write((uint8_t *)buf, n * 2 * sizeof(int16_t));
        done += n;
    }
}

// Play an embedded mono PCM clip (already at SND_RATE) as I2S stereo.
static void play_pcm(const int16_t* pcm, uint32_t n) {
    if (!ready || !pcm || !n) return;
    int16_t buf[256 * 2];
    uint32_t done = 0;
    while (done < n) {
        uint32_t c = n - done;
        if (c > 256) c = 256;
        for (uint32_t i = 0; i < c; i++) {
            int16_t s = pcm[done + i];
            buf[i * 2] = s;
            buf[i * 2 + 1] = s;
        }
        i2s.write((uint8_t*)buf, c * 2 * sizeof(int16_t));
        done += c;
    }
}

void sound_chime(void) {
    if (!ready) return;
    play_tone(988.0f, 70);    // B5
    play_tone(1318.0f, 90);   // E6 — gentle rising "ding"
}

void sound_alert(void) {
    if (!ready) return;
    play_tone(660.0f, 110);
    delay(40);
    play_tone(523.0f, 140);   // falling tone — attention
}

// Play the real game-sounds (warcraft) voice clip for the event.
// Falls back to a synthesized tone if the event name is unknown.
void sound_event(const char* ev) {
    if (!ready || !ev) return;
    if      (strcmp(ev, "session-start") == 0)    play_pcm(snd_session_start,    snd_session_start_len);
    else if (strcmp(ev, "task-acknowledge") == 0) play_pcm(snd_task_acknowledge, snd_task_acknowledge_len);
    else if (strcmp(ev, "task-complete") == 0)    play_pcm(snd_task_complete,    snd_task_complete_len);
    else if (strcmp(ev, "error") == 0)            play_pcm(snd_error,            snd_error_len);
    else if (strcmp(ev, "permission") == 0)       play_pcm(snd_permission,       snd_permission_len);
    else                                          sound_chime();
}
