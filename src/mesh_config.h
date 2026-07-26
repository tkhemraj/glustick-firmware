#pragma once
#ifdef TRANSPORT_MESHTASTIC

#include <stdint.h>

// ── GluStic private channel ───────────────────────────────────────────────────
// AES-128 key shared between both GluStic devices only.
// Community nodes relay LoRa packets but cannot decrypt content.
// MQTT messages go to topic  msh/2/e/GluStic/!{hex}  (private).
static const uint8_t GLUSTICK_PSK[16] = {
    0x2f, 0x8e, 0x5c, 0xa1, 0x3d, 0x7b, 0x94, 0xf6,
    0x01, 0xe8, 0x6d, 0x52, 0xc9, 0x4a, 0xbb, 0x37
};
static const char GLUSTICK_CHANNEL[] = "GluStic";

static inline uint8_t glustick_chan_hash() {
    uint8_t h = 0;
    for (const char *c = GLUSTICK_CHANNEL; *c; c++) h = (h + (uint8_t)*c) & 0xFF;
    for (int i = 0; i < 16; i++)                   h = (h + GLUSTICK_PSK[i]) & 0xFF;
    return h;
}

// ── Meshtastic public LongFast channel ────────────────────────────────────────
// Used ONLY for NodeInfo shadow-publish so the device appears on the
// Meshtastic community map (meshtastic.org/d/map).
// Derived from the Meshtastic default key "AQ==" (base64 of 0x01).
// Text messages are NEVER sent on this channel — only identity/presence.
static const uint8_t LONGFAST_PSK[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01
};
static const char LONGFAST_CHANNEL[] = "LongFast";

static inline uint8_t longfast_chan_hash() {
    uint8_t h = 0;
    for (const char *c = LONGFAST_CHANNEL; *c; c++) h = (h + (uint8_t)*c) & 0xFF;
    for (int i = 0; i < 16; i++)                   h = (h + LONGFAST_PSK[i]) & 0xFF;
    return h;
}

#endif // TRANSPORT_MESHTASTIC
