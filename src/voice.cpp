#if defined(FEATURE_VOICE) && defined(BOARD_TDECK) && defined(TRANSPORT_P2P)

#include "voice.h"
#include "audio.h"
#include "radio_p2p.h"
#include "voice_jbuf.h"
#include <codec2.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#ifndef PTT_PIN
#  define PTT_PIN 0        // BOOT pin / trackball click, active LOW
#endif
#ifndef VOICE_CODEC2_MODE
#  define VOICE_CODEC2_MODE CODEC2_MODE_700C
#endif
#ifndef VOICE_FRAMES_PER_PKT
#  define VOICE_FRAMES_PER_PKT 9
#endif

// P2P payload budget (keep in sync with radio_p2p.cpp P2P_BODY_MAX = 38).
#define VOICE_BODY_MAX 38

struct VoicePkt {
    uint8_t len;
    uint8_t data[VOICE_BODY_MAX];
};

static struct CODEC2 *s_enc = nullptr;
static struct CODEC2 *s_dec = nullptr;
static int  s_samples_per_frame = 320;   // resolved from the codec at init
static int  s_bytes_per_frame   = 4;
static int  s_frames_per_pkt     = VOICE_FRAMES_PER_PKT;

static QueueHandle_t s_txq = nullptr;    // core0 -> core1 (encoded groups out)
static QueueHandle_t s_rxq = nullptr;    // core1 -> core0 (encoded groups in)

static volatile bool s_talking = false;
static VoicePkt s_tx_hold;               // a group that couldn't send yet
static bool     s_tx_hold_busy = false;

// ── Radio RX callback (runs on core 1, from lora_loop) ───────────────────────
static void on_voice_rx(const uint8_t *data, uint8_t len) {
    if (!s_rxq) return;
    VoicePkt p;
    p.len = (len > VOICE_BODY_MAX) ? VOICE_BODY_MAX : len;
    if (p.len) memcpy(p.data, data, p.len);
    // Non-blocking: if the audio task is behind, drop this group (voice is
    // loss-tolerant — a dropped 360 ms beats stalling the radio).
    xQueueSend(s_rxq, &p, 0);
}

// ── Encode one captured frame and accumulate into the outgoing group ─────────
static void tx_flush_group(uint8_t *accum, int &frames) {
    if (frames <= 0) return;
    VoicePkt p;
    p.len = (uint8_t)(frames * s_bytes_per_frame);
    memcpy(p.data, accum, p.len);
    xQueueSend(s_txq, &p, 0);   // drop if full rather than block the mic
    frames = 0;
}

// ── Audio task (core 0): owns mic, codec, speaker ────────────────────────────
static void audio_task(void *) {
    int16_t pcm[320];
    uint8_t tx_accum[VOICE_BODY_MAX];
    int     tx_frames = 0;
    bool    was_talking = false;

    for (;;) {
        bool ptt = (digitalRead(PTT_PIN) == LOW);

        if (ptt) {
            // ── TALK ──────────────────────────────────────────────────────
            if (!was_talking) {
                was_talking = true;
                tx_frames = 0;
                // flush any stale RX so we don't play old audio after talking
                VoicePkt junk;
                while (xQueueReceive(s_rxq, &junk, 0) == pdTRUE) {}
                jbuf_reset();
            }
            if (audio_mic_ok()) {
                size_t got = audio_mic_read(pcm, s_samples_per_frame);
                if ((int)got == s_samples_per_frame) {
                    codec2_encode(s_enc, tx_accum + tx_frames * s_bytes_per_frame, pcm);
                    tx_frames++;
                    if (tx_frames >= s_frames_per_pkt) tx_flush_group(tx_accum, tx_frames);
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(20));  // no mic — idle while held
            }
            s_talking = true;

        } else {
            // ── LISTEN ────────────────────────────────────────────────────
            if (was_talking) {
                was_talking = false;
                s_talking   = false;
                tx_flush_group(tx_accum, tx_frames);  // send partial group
                VoicePkt end; end.len = 0;             // end-of-talk marker
                xQueueSend(s_txq, &end, 0);
            }

            // Drain received groups -> decode -> jitter buffer
            VoicePkt rp;
            while (xQueueReceive(s_rxq, &rp, 0) == pdTRUE) {
                if (rp.len == 0) continue;             // end marker: just let jbuf drain
                int nframes = rp.len / s_bytes_per_frame;
                for (int f = 0; f < nframes; f++) {
                    codec2_decode(s_dec, pcm, rp.data + f * s_bytes_per_frame);
                    jbuf_push(pcm, s_samples_per_frame);
                }
            }

            // Feed the speaker one frame-worth (paces the loop at real time)
            jbuf_pop(pcm, s_samples_per_frame);
            audio_speaker_write(pcm, s_samples_per_frame);
        }
    }
}

bool voice_init() {
    s_enc = codec2_create(VOICE_CODEC2_MODE);
    s_dec = codec2_create(VOICE_CODEC2_MODE);
    if (!s_enc || !s_dec) return false;

    s_samples_per_frame = codec2_samples_per_frame(s_enc);
    s_bytes_per_frame   = codec2_bytes_per_frame(s_enc);
    if (s_samples_per_frame > 320) s_samples_per_frame = 320;  // buffer bound

    // How many whole frames fit one LoRa payload.
    int fit = VOICE_BODY_MAX / s_bytes_per_frame;
    s_frames_per_pkt = (VOICE_FRAMES_PER_PKT < fit) ? VOICE_FRAMES_PER_PKT : fit;
    if (s_frames_per_pkt < 1) s_frames_per_pkt = 1;

    if (!audio_init()) return false;
    jbuf_reset();

    s_txq = xQueueCreate(8, sizeof(VoicePkt));
    s_rxq = xQueueCreate(8, sizeof(VoicePkt));
    if (!s_txq || !s_rxq) return false;

    pinMode(PTT_PIN, INPUT_PULLUP);
    lora_set_voice_cb(on_voice_rx);

    // Audio task on core 0 (Arduino loop runs on core 1 here).
    BaseType_t rc = xTaskCreatePinnedToCore(
        audio_task, "voice_audio", 16384, nullptr, 2, nullptr, 0);
    return rc == pdPASS;
}

void voice_loop() {
    // Send one pending/held group per call; hold it if the radio is busy.
    if (s_tx_hold_busy) {
        if (lora_send_raw(s_tx_hold.data, s_tx_hold.len)) s_tx_hold_busy = false;
        return;
    }
    VoicePkt p;
    if (s_txq && xQueueReceive(s_txq, &p, 0) == pdTRUE) {
        if (!lora_send_raw(p.data, p.len)) { s_tx_hold = p; s_tx_hold_busy = true; }
    }
}

bool voice_is_talking() { return s_talking; }

#endif
