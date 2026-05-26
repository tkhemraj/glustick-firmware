#pragma once
#include <Arduino.h>
#include "config.h"
#include "frame.h"

// Callback invoked when a downlink message arrives (already decoded/reassembled).
typedef void (*LoRaDownlinkCb)(const char *body);

// Initialise LMIC with keys from NVS. Call after provisioning_load().
void lora_init(
    const char *dev_eui_hex,  // 16 hex chars e.g. "70B3D57ED0049C4F"
    const char *app_eui_hex,
    const char *app_key_hex,
    LoRaDownlinkCb downlink_cb
);

// Must be called from loop() — runs the LMIC state machine.
void lora_loop();

// Queue a message for uplink. Chunks automatically if > 44 bytes.
// Returns false if LMIC TX is busy.
bool lora_send(const char *body, uint16_t msg_id);

// Send a ping frame (keepalive).
void lora_ping(uint16_t msg_id);

// True after OTAA join succeeds.
bool lora_is_joined();

// Last RSSI from a received downlink (0 if never received one).
int lora_last_rssi();
