#pragma once
// ── Push-to-talk voice over LoRa (Codec2) for the T-Deck ─────────────────────
// Half-duplex walkie-talkie: hold PTT_PIN (trackball click / GPIO0) to talk,
// release to listen. Speech is Codec2-compressed, chopped into frame-groups,
// and carried as FLAG_VOICE packets on the same SX1262 P2P link as text.
//
// Threading: a dedicated audio task on core 0 owns the mic, the codec, and the
// speaker. The radio stays on core 1 (main loop). Encoded TX frame-groups cross
// core0 -> core1 via a FreeRTOS queue (drained in voice_loop); received groups
// cross core1 -> core0 via another queue (the LoRa voice callback fills it).
#if defined(FEATURE_VOICE) && defined(BOARD_TDECK) && defined(TRANSPORT_P2P)

#include <Arduino.h>

// Bring up the codec, audio I/O, queues and the core-0 audio task, and register
// the radio voice callback. Call once in setup() AFTER lora_init().
bool voice_init();

// Call from loop() on the radio core: drains encoded groups to the radio.
void voice_loop();

// True while the local user is holding PTT (transmitting).
bool voice_is_talking();

#endif
