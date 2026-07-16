#include <Arduino.h>
#include "config.h"
#include "display.h"
#include "provisioning.h"
#include "lorawan.h"
#include "wifi_sync.h"
#include "message_queue.h"
#include "frame.h"
#ifdef BOARD_TDECK
#  include "keyboard.h"
#endif

// ── Device context (loaded from NVS after provisioning) ──────────────────────
static char g_dev_eui[17]    = {};
static char g_app_eui[17]    = {};
static char g_app_key[33]    = {};
static char g_server_url[128] = {};
static char g_parent_tok[256] = {};
static char g_wifi_ssid[64]  = {};
static char g_wifi_pass[64]  = {};
static char g_kid_name[32]   = {};
static char g_server_cert[2048] = {};

static DeviceState g_state = STATE_PROVISIONING;

static unsigned long g_last_ping_ms = 0;

#ifdef BOARD_TDECK
// Compose buffer — accumulates keyboard input until Enter is pressed
static char     g_compose[COMPOSE_BUF_MAX] = {};
static uint16_t g_compose_len              = 0;
static bool     g_compose_dirty            = false;
#endif

// ── Battery reading ───────────────────────────────────────────────────────────
static int read_battery_pct() {
    // Heltec V3 has a voltage divider on VBAT_PIN (GPIO 1).
    // ADC reads 0–4095, 3.3 V reference, divider gives roughly 4.2 V full.
    int raw = analogRead(VBAT_PIN);
    float v = (raw / 4095.0f) * 3.3f * 2.0f;  // ×2 for divider
    // 3.0 V = 0%, 4.2 V = 100%
    int pct = (int)((v - 3.0f) / 1.2f * 100.0f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

// ── Callbacks ─────────────────────────────────────────────────────────────────
static void on_lora_downlink(const char *body) {
    queue_push_inbound(queue_next_msg_id(), body);
    display_show_message("Incoming", body);
    delay(3000);
}

static void on_wifi_inbound(const char *body) {
    queue_push_inbound(queue_next_msg_id(), body);
    display_show_message("WiFi msg", body);
    delay(3000);
}

// ── State transitions ─────────────────────────────────────────────────────────
static void enter_state(DeviceState next) {
    g_state = next;
    switch (next) {
        case STATE_PROVISIONING:
            // QR display is rendered inside provisioning_run() after derive_defaults
            break;
        case STATE_JOINING:
            lora_init(g_dev_eui, g_app_eui, g_app_key, on_lora_downlink);
            display_show_joining();
            break;
        case STATE_LORA_CONNECTED:
            display_show_idle(g_kid_name, lora_last_rssi(), read_battery_pct(), queue_outbound_count());
            break;
        case STATE_WIFI_CONNECTED:
            display_show_wifi_sync();
            break;
        case STATE_IDLE:
            display_show_idle(g_kid_name, lora_last_rssi(), read_battery_pct(), queue_outbound_count());
            break;
        case STATE_ERROR:
            break;
    }
}

// ── Arduino entry points ──────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);

    display_init();  // also drives TDECK_POWERON_PIN HIGH on T-Deck
    display_show_boot();
    delay(1500);

#ifdef BOARD_TDECK
    keyboard_init();
#endif

    queue_init();

    // Derive factory defaults (DevEUI from MAC) before showing the provisioning QR
    provisioning_derive_defaults();

    if (!provisioning_is_complete()) {
        enter_state(STATE_PROVISIONING);
        if (!provisioning_run()) {
            display_show_error("Setup timed out.\nPower cycle to retry.");
            enter_state(STATE_ERROR);
            return;
        }
    }

    if (!provisioning_load(
            g_dev_eui, g_app_eui, g_app_key,
            g_server_url, g_parent_tok,
            g_wifi_ssid, g_wifi_pass, g_kid_name, g_server_cert)) {
        display_show_error("NVS read failed");
        enter_state(STATE_ERROR);
        return;
    }

    // Try WiFi first — faster and cheaper than LoRa for syncing
    bool has_wifi = wifi_sync_init(
        g_wifi_ssid, g_wifi_pass,
        g_server_url, g_parent_tok,
        g_server_cert,
        on_wifi_inbound
    );

    // Always bring up LoRaWAN — it's the fallback when WiFi is gone
    enter_state(STATE_JOINING);

    if (has_wifi) {
        wifi_sync_flush();  // drain any queued messages immediately
        enter_state(STATE_WIFI_CONNECTED);
    }
}

void loop() {
    if (g_state == STATE_ERROR) {
        delay(1000);
        return;
    }

    // Run LMIC tick
    lora_loop();

    // State transitions based on connectivity
    bool wifi_up = wifi_sync_connected();
    bool lora_up = lora_is_joined();

    if (g_state == STATE_JOINING && lora_up) {
        enter_state(wifi_up ? STATE_WIFI_CONNECTED : STATE_LORA_CONNECTED);
    }

    // WiFi sync
    if (wifi_up) {
        wifi_sync_loop();
    }

    // LoRa uplink for queued outbound messages
    if (lora_up && !wifi_up) {
        QueuedMessage msg;
        if (queue_pop_outbound(&msg)) {
            display_show_sending(queue_outbound_count());
            if (lora_send(msg.body, msg.id)) {
                queue_mark_acked(msg.id);
            }
        }
    }

    // Periodic keepalive ping over LoRa
    if (lora_up && (millis() - g_last_ping_ms) >= PING_INTERVAL_MS) {
        lora_ping(queue_next_msg_id());
        g_last_ping_ms = millis();
    }

    // Refresh idle display every 30 s
    static unsigned long last_display_ms = 0;
    if (g_state == STATE_LORA_CONNECTED || g_state == STATE_IDLE) {
        if (millis() - last_display_ms >= 30000) {
            display_show_idle(g_kid_name, lora_last_rssi(), read_battery_pct(), queue_outbound_count());
            last_display_ms = millis();
        }
    }

#ifdef BOARD_TDECK
    // Keyboard input: accumulate chars, send on Enter, backspace to delete.
    // Only active when we have a working connection and are in a steady state.
    if (g_state == STATE_LORA_CONNECTED || g_state == STATE_WIFI_CONNECTED || g_state == STATE_IDLE) {
        char k = keyboard_read();
        if (k != 0) {
            if (k == '\r' || k == '\n') {
                // Send the message if compose buffer is non-empty
                if (g_compose_len > 0) {
                    g_compose[g_compose_len] = '\0';
                    uint16_t id = queue_next_msg_id();
                    queue_push_outbound(id, g_compose);
                    g_compose_len = 0;
                    g_compose[0]  = '\0';
                    // Flush immediately over WiFi; LoRa send happens in main loop above
                    if (wifi_up) wifi_sync_flush();
                }
                display_show_idle(g_kid_name, lora_last_rssi(), read_battery_pct(), queue_outbound_count());
            } else if (k == '\b' && g_compose_len > 0) {
                // Backspace
                g_compose[--g_compose_len] = '\0';
                g_compose_dirty = true;
            } else if (k >= 0x20 && g_compose_len < COMPOSE_BUF_MAX - 1) {
                // Printable character
                g_compose[g_compose_len++] = k;
                g_compose[g_compose_len]   = '\0';
                g_compose_dirty = true;
            }

            if (g_compose_dirty) {
                display_show_compose(g_compose);
                g_compose_dirty = false;
            }
        }
    }
#endif
}
