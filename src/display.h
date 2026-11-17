#pragma once
#include <Arduino.h>
#include "config.h"

void display_init();
void display_show_boot();
void display_show_provisioning(const char *ble_name);
void display_show_joining();
void display_show_idle(const char *kid_name, int rssi, int battery_pct, uint8_t pending);
void display_show_message(const char *from, const char *body);
void display_show_sending(uint8_t pending);
void display_show_wifi_sync();
void display_show_error(const char *msg);
void display_set_lora_rssi(int rssi);
void display_show_provisioning_qr(const char *dev_eui, const char *app_eui, const char *app_key, const char *ble_name, uint32_t pin_code);
// T-Deck: updates the compose strip at the bottom of the screen with current draft.
// No-op on Heltec V3.
void display_show_compose(const char *draft);

// Add a message to the persistent chat history and redraw the message area.
// outgoing=true for messages this device sent, false for received messages.
void display_push_message(const char *from, const char *body, bool outgoing);

// Restore the status bar after a temporary overlay (e.g. display_show_sending).
void display_refresh_status();

// Scroll the message list.  Positive = scroll toward older messages, negative = toward newer.
// No-op on Heltec V3.
void display_scroll(int delta);

// Handle a touch event at display coordinates (x, y).  No-op on Heltec V3.
void display_touch(int16_t x, int16_t y);

// Scan I2C bus and show found addresses on screen for `hold_ms` milliseconds.
// Useful for diagnosing what's actually present on the bus.
void display_show_i2c_scan(uint32_t hold_ms = 4000);

// ── GluStic Fun: low-level draw helpers used by the tile map renderer ─────────
#ifdef GLUSTIC_FUN
void display_draw_pixels(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *pixels);
void display_fill_rect(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t color);
void display_fill_circle(int16_t x, int16_t y, uint8_t r, uint16_t color);
#endif

// ── Meshtastic WiFi setup (T-Deck only) ───────────────────────────────────────
// Draw the Wi-Fi credentials entry screen.
// active_field: 0=SSID highlighted, 1=password highlighted, -1=status-only mode.
// status: optional status string shown below the fields (nullptr = none).
void display_show_wifi_setup(const char *ssid, const char *pass,
                              int active_field, const char *status);

// Update WiFi / MQTT indicators in the status bar.
// Call after connection state changes, then call display_refresh_status().
void display_set_wifi_status(int wifi_rssi, bool mqtt_connected);
// Show which paths carried an outgoing message.
// Call after lora_send(), then delay(600), then display_refresh_status().
void display_show_sent_path(bool via_lora, bool via_wifi);

// P2P pairing screens (T-Deck only; no-ops on Heltec V3).
void display_show_p2p_mode_select();
void display_show_p2p_host(const char *code);             // code = 6-char uppercase hex
void display_show_p2p_join(const char *typed, int len);   // typed = chars entered so far
void display_show_p2p_paired();

// Heltec repeater. listening is true until the first family packet is heard.
void display_show_repeater(bool listening, uint32_t net_id, int rssi,
                           uint32_t repeats, int battery_pct);
