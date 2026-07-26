#pragma once
#ifdef TRANSPORT_MESHTASTIC

#include <Arduino.h>

// Invoked when a text message arrives via MQTT (WiFi path).
typedef void (*WifiMeshRxCb)(const char *body);

// Initialise WiFi+MQTT subsystem.  Call once in setup() before wifi_mesh_begin().
// node_id:   this device's 32-bit Meshtastic node ID (lower 32 bits of eFuse MAC)
// psk:       16-byte AES PSK matching radio_meshtastic.cpp
// channel:   channel name string (e.g. "GluStic")
// chan_hash:  1-byte channel hash
// cb:        invoked for every inbound text message
void wifi_mesh_init(uint32_t node_id, const uint8_t psk[16],
                    const char *channel, uint8_t chan_hash,
                    WifiMeshRxCb cb);

// Load WiFi credentials from NVS (namespace "gsf_wifi").
// Returns true if a non-empty SSID was found.
bool wifi_mesh_load_creds(char *ssid_out, char *pass_out,
                           size_t ssid_max, size_t pass_max);

// Persist WiFi credentials to NVS.
void wifi_mesh_save_creds(const char *ssid, const char *pass);

// Start WiFi + MQTT connect in a dedicated FreeRTOS task (20KB stack).
// Returns immediately — use wifi_mesh_ready() to poll for completion.
// WiFi RF calibration overflows the default 8KB loopTask stack on first boot,
// so we give it its own task.
void wifi_mesh_begin(const char *ssid, const char *pass);
bool wifi_mesh_ready();  // true once the background init task finishes

// Must be called every loop() tick — runs the MQTT client and auto-reconnects.
// Safe to call before wifi_mesh_ready() — no-op until connected.
void wifi_mesh_loop();

// Publish an already-encrypted Meshtastic payload over MQTT.
// Pass the same (to, from, pkt_id, encrypted, elen) used for the LoRa TX.
// No-op if MQTT is not connected.
void wifi_mesh_send(uint32_t to, uint32_t from, uint32_t pkt_id,
                     const uint8_t *encrypted, size_t elen, uint8_t hop_limit);

// Dedup ring: returns true if pkt_id was already seen (and adds it to the ring).
// Call in the LoRa RX path to suppress re-delivery of MQTT-forwarded packets.
bool wifi_mesh_dedup(uint32_t pkt_id);

// Publish a GPS position on the public LongFast channel so this device
// appears with a location dot on the Meshtastic community map.
// No-op if MQTT is not connected or lat/lon are both zero.
void wifi_mesh_publish_position(float lat, float lon);

bool wifi_mesh_wifi_up();   // true if WiFi associated
bool wifi_mesh_mqtt_up();   // true if MQTT broker connected
int  wifi_mesh_rssi();      // WiFi RSSI in dBm, 0 if not connected

#endif // TRANSPORT_MESHTASTIC
