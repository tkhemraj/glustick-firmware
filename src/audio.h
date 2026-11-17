#pragma once
// ── Audio I/O for the LilyGo T-Deck ──────────────────────────────────────────
// Speaker: I2S0 (MASTER/TX) -> onboard I2S amplifier.
// Mic:     I2S1 (MASTER/RX) <- ES7210 4-ch ADC (configured over I2C).
// Everything is 8 kHz / 16-bit / mono — Codec2's native format, so no resample.
//
// Only built for the T-Deck voice build (FEATURE_VOICE + BOARD_TDECK).
#if defined(FEATURE_VOICE) && defined(BOARD_TDECK)

#include <Arduino.h>

#define AUDIO_SAMPLE_RATE 8000

// Bring up both I2S peripherals + the ES7210 mic. Returns false on hard failure
// (mic ADC not found / I2S install error). Speaker may still work if only the
// mic failed — check audio_mic_ok().
bool audio_init();

// True if the ES7210 mic initialised. Speaker-only (loopback/tone) still works
// when this is false.
bool audio_mic_ok();

// Read exactly `samples` 16-bit mono samples from the mic (blocking, bounded).
// Returns the number of samples actually read.
size_t audio_mic_read(int16_t *buf, size_t samples);

// Write `samples` 16-bit mono samples to the speaker (blocking until DMA room).
void audio_speaker_write(const int16_t *buf, size_t samples);

// Push silence through the speaker DMA (de-click when a talk burst ends).
void audio_speaker_silence(size_t samples);

#endif // FEATURE_VOICE && BOARD_TDECK
