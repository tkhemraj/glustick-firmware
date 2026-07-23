#pragma once
#ifdef TRANSPORT_MESHTASTIC

#include <Arduino.h>
#include "config.h"

// Callback invoked when a text message arrives from the mesh.
typedef void (*LoRaDownlinkCb)(const char *body);

// Initialise the SX1262 and begin listening on the Meshtastic LongFast channel.
// dev_eui_hex / app_eui_hex are unused.
// app_key_hex: 32 hex chars (16 bytes) used as the AES-128 channel PSK.
//   Leave empty or set to all-zeros to use the Meshtastic public default key.
void lora_init(
    const char *dev_eui_hex,
    const char *app_eui_hex,
    const char *app_key_hex,
    LoRaDownlinkCb cb
);

// Must be called from loop() — processes RX/TX completions.
void lora_loop();

// Broadcast a text message on the mesh (TEXT_MESSAGE_APP, port 1).
// Returns false if TX is in progress.
bool lora_send(const char *body, uint16_t msg_id);

// No-op: Meshtastic does not use keepalive pings.
void lora_ping(uint16_t msg_id);

// True once the radio is initialised (no OTAA join needed).
bool lora_is_joined();

// RSSI of the last received packet (dBm), 0 if none yet.
int lora_last_rssi();

#endif // TRANSPORT_MESHTASTIC
