#pragma once
#include <Arduino.h>

// ── RX jitter buffer for LoRa voice ──────────────────────────────────────────
// 8 kHz / 16-bit / mono PCM. Lock-free single-producer (radio core) /
// single-consumer (speaker core) ring: the producer owns `head`, the consumer
// owns `tail`. Smooths bursty LoRa packet arrival into continuous playback.
// Drafted by the local coder; overflow + underrun paths corrected here.

// Reset to empty, not-yet-playing.
void   jbuf_reset();
// Producer (radio core): copy n mono samples in. Drops NEW samples that don't
// fit when full (never touches the consumer's tail).
void   jbuf_push(const int16_t *pcm, size_t n);
// Consumer (speaker core): fill out[0..n) from the buffer. Returns n always;
// any shortfall is zero-filled (underrun => silence, never a click).
size_t jbuf_pop(int16_t *out, size_t n);
// False until the prebuffer threshold has accumulated; stays true until the
// buffer fully drains to empty (then false again, so the next burst re-primes).
bool   jbuf_playing();
// Samples currently buffered.
size_t jbuf_level();
