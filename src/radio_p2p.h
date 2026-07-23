#pragma once
#ifdef TRANSPORT_P2P

#include <Arduino.h>
#include "config.h"

// Callback invoked when an inbound message arrives from a peer.
typedef void (*LoRaDownlinkCb)(const char *body);

// Initialise the SX1262 and begin listening.
// dev_eui_hex / app_eui_hex are unused in P2P mode.
// app_key_hex: first 4 bytes (8 hex chars) become the 32-bit network ID —
//   provision both T-Decks with the same app_key to pair them.
void lora_init(
    const char *dev_eui_hex,
    const char *app_eui_hex,
    const char *app_key_hex,
    LoRaDownlinkCb cb
);

// Must be called from loop() — processes RX/TX completions.
void lora_loop();

// Broadcast body to all peers on the same network ID.
// Returns false if a TX is already in progress.
bool lora_send(const char *body, uint16_t msg_id);

// Send a short keepalive packet.
void lora_ping(uint16_t msg_id);

// True once the radio is initialised (no OTAA join needed for P2P).
bool lora_is_joined();

// RSSI of the last received packet (dBm), 0 if none yet.
int lora_last_rssi();

#endif // TRANSPORT_P2P
