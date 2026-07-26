#include <Arduino.h>
#include "config.h"
#include "display.h"
#include "provisioning.h"
#if defined(TRANSPORT_P2P)
#  include "radio_p2p.h"
#  include "p2p_pair.h"
#elif defined(TRANSPORT_MESHTASTIC)
#  include "radio_meshtastic.h"
#  include "wifi_mesh.h"
#  include "mesh_config.h"
#else
#  include "lorawan.h"   // default: self-hosted LoRaWAN via MCCI LMIC
#endif
#include "wifi_sync.h"
#include "message_queue.h"
#include "frame.h"
#ifdef BOARD_TDECK
#  include "keyboard.h"
#  include "trackball.h"
#  include "touch.h"
#endif
#ifdef GLUSTIC_FUN
#  include "gps.h"
#  include "map_tiles.h"
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

#ifdef GLUSTIC_FUN
static GpsCoord  g_own_pos  = {};   // our last GPS fix
static GpsCoord  g_peer_pos = {};   // peer's last known position (from LoRa packets)
static bool      g_map_view = false; // false = chat, true = map
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
#ifdef TRANSPORT_MESHTASTIC
    display_push_message("Radio", body, false);
#else
    display_push_message("Remote", body, false);
#endif
}

#ifdef TRANSPORT_MESHTASTIC
static void on_wifi_downlink(const char *body) {
    queue_push_inbound(queue_next_msg_id(), body);
    display_push_message("WiFi", body, false);
}
#endif

static void on_wifi_inbound(const char *body) {
    queue_push_inbound(queue_next_msg_id(), body);
    display_push_message("Remote", body, false);
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
#ifdef BOARD_TDECK
    // Power-on latch — must be the very first GPIO action.
    // The PMIC cuts power if GPIO10 is not HIGH within milliseconds of reset.
    pinMode(TDECK_POWERON_PIN, OUTPUT);
    digitalWrite(TDECK_POWERON_PIN, HIGH);
    // Backlight on immediately so a white screen confirms we're running.
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
#endif

    Serial.begin(115200);

    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);

    display_init();  // also drives TDECK_POWERON_PIN HIGH on T-Deck
    display_show_boot();
    delay(1500);

#ifdef BOARD_TDECK
    keyboard_init();
    trackball_init();
    if (!touch_init()) {
        Serial.println("GT911 touch not found");
    }
#endif

    queue_init();

#ifdef GLUSTIC_FUN
    gps_init();
    if (!map_tiles_init()) {
        // SD missing — map view will show an error when opened, chat still works
    }
#endif

#ifdef TRANSPORT_P2P
    // P2P mode: pair the two devices directly via LoRa — no server, no BLE, no WiFi.

    // Force re-pair window: if the keyboard is already known-good, pressing any key
    // within 500 ms of boot clears the saved pairing so HOST/JOIN runs again.
    // Useful when one device is reflashed and the other still has the old key.
#ifdef BOARD_TDECK
    if (p2p_pair_is_complete()) {
        unsigned long deadline = millis() + 500;
        while (millis() < deadline) {
            if (keyboard_read() != 0) { p2p_pair_clear(); break; }
            delay(20);
        }
    }
#endif

    queue_clear();  // wipe any LoRaWAN-era messages that survived a reflash in NVS

    if (!p2p_pair_is_complete()) {
        if (!p2p_pair_run()) {
            display_show_error("Pairing failed.\nPower cycle to retry.");
            enter_state(STATE_ERROR);
            return;
        }
    }
    p2p_pair_load_key(g_app_key);
    {
        uint64_t m = ESP.getEfuseMac();
        snprintf(g_kid_name, sizeof(g_kid_name), "Dev-%04X", (uint16_t)(m & 0xFFFF));
    }
    lora_init(g_dev_eui, g_app_eui, g_app_key, on_lora_downlink);
    enter_state(STATE_LORA_CONNECTED);
#elif defined(TRANSPORT_MESHTASTIC)
    // Meshtastic: private channel with community relay + optional WiFi/MQTT bridge.
    queue_clear();
    {
        uint64_t m = ESP.getEfuseMac();
        snprintf(g_kid_name, sizeof(g_kid_name), "Dev-%04X", (uint16_t)(m & 0xFFFF));
    }

    // ── WiFi credentials setup ───────────────────────────────────────────────
    {
        uint32_t mesh_node_id = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF);
        uint8_t  chan_hash    = longfast_chan_hash();
        wifi_mesh_init(mesh_node_id, LONGFAST_PSK, LONGFAST_CHANNEL,
                       chan_hash, on_wifi_downlink);

        // Build-flag creds always win — they overwrite whatever is in NVS.
        // This prevents stale keyboard-entered garbage from persisting.
#if defined(MESH_WIFI_SSID) && defined(MESH_WIFI_PASS)
        if (strlen(MESH_WIFI_SSID) > 0) {
            strncpy(g_wifi_ssid, MESH_WIFI_SSID, sizeof(g_wifi_ssid) - 1);
            strncpy(g_wifi_pass, MESH_WIFI_PASS, sizeof(g_wifi_pass) - 1);
            wifi_mesh_save_creds(g_wifi_ssid, g_wifi_pass);
            Serial.printf("[wifi] using baked creds: ssid=%s\n", g_wifi_ssid);
        }
#else
        wifi_mesh_load_creds(g_wifi_ssid, g_wifi_pass,
                              sizeof(g_wifi_ssid), sizeof(g_wifi_pass));
        Serial.printf("[wifi] loaded from NVS: ssid=%s\n", g_wifi_ssid);
#endif

        // Start WiFi+MQTT in a dedicated 20KB task — WiFi RF calibration
        // overflows the default 8KB loopTask stack on cold boot.
        if (g_wifi_ssid[0]) {
            display_show_wifi_setup(g_wifi_ssid, g_wifi_pass, -1, "Connecting...");
            wifi_mesh_begin(g_wifi_ssid, g_wifi_pass);
            // Wait up to 20 s for the background task to finish
            unsigned long deadline = millis() + 20000;
            while (!wifi_mesh_ready() && millis() < deadline) delay(100);
            display_set_wifi_status(wifi_mesh_rssi(), wifi_mesh_mqtt_up());
            display_show_wifi_setup(g_wifi_ssid, g_wifi_pass, -1,
                                     wifi_mesh_mqtt_up()
                                         ? "Connected to Meshtastic MQTT"
                                         : (wifi_mesh_wifi_up()
                                                ? "Wi-Fi OK, MQTT failed"
                                                : "Wi-Fi failed - LoRa only"));
            delay(1500);
        }
    }

    lora_init("", "", "", on_lora_downlink);
    enter_state(STATE_LORA_CONNECTED);
#else
    // LoRaWAN: full BLE provisioning → server + WiFi setup.
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

    bool has_wifi = wifi_sync_init(
        g_wifi_ssid, g_wifi_pass,
        g_server_url, g_parent_tok,
        g_server_cert,
        on_wifi_inbound
    );

    enter_state(STATE_JOINING);

    if (has_wifi) {
        wifi_sync_flush();
        enter_state(STATE_WIFI_CONNECTED);
    }
#endif
}

void loop() {
    if (g_state == STATE_ERROR) {
        delay(1000);
        return;
    }

    lora_loop();

#ifdef TRANSPORT_MESHTASTIC
    // WiFi+MQTT tick — reconnects automatically if disconnected
    wifi_mesh_loop();

    // Refresh WiFi/MQTT indicators in status bar every 5 s
    static unsigned long last_wifi_status_ms = 0;
    if (millis() - last_wifi_status_ms >= 5000) {
        last_wifi_status_ms = millis();
        display_set_wifi_status(wifi_mesh_rssi(), wifi_mesh_mqtt_up());
        display_refresh_status();
    }
#endif

    // State transitions based on connectivity
#if !defined(TRANSPORT_P2P) && !defined(TRANSPORT_MESHTASTIC)
    bool wifi_up = wifi_sync_connected();
#else
    bool wifi_up = false;
#endif
    bool lora_up = lora_is_joined();

    if (g_state == STATE_JOINING && lora_up) {
        enter_state(wifi_up ? STATE_WIFI_CONNECTED : STATE_LORA_CONNECTED);
    }

    // WiFi sync (LoRaWAN builds only — Meshtastic uses its own MQTT path)
#if !defined(TRANSPORT_P2P) && !defined(TRANSPORT_MESHTASTIC)
    if (wifi_up) {
        wifi_sync_loop();
    }
#endif

#if !defined(TRANSPORT_P2P) && !defined(TRANSPORT_MESHTASTIC)
    // LoRa uplink for queued outbound messages (LoRaWAN only)
    if (lora_up && !wifi_up) {
        QueuedMessage msg;
        if (queue_pop_outbound(&msg)) {
            display_show_sending(queue_outbound_count());
            if (lora_send(msg.body, msg.id)) {
                queue_mark_acked(msg.id);
            }
            display_refresh_status();
        }
    }
#endif

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

#ifdef GLUSTIC_FUN
    gps_loop();
    g_own_pos = gps_get();
#ifdef TRANSPORT_MESHTASTIC
    if (g_own_pos.valid) {
        static unsigned long last_pos_ms = 0;
        if (millis() - last_pos_ms >= 60000) {
            last_pos_ms = millis();
            wifi_mesh_publish_position(g_own_pos.lat, g_own_pos.lon);
        }
    }
#endif
    if (g_map_view) {
        static unsigned long last_map_ms = 0;
        if (millis() - last_map_ms >= 2000) {  // redraw every 2s
            map_tiles_draw(g_own_pos, g_peer_pos);
            last_map_ms = millis();
        }
    }
#endif

#ifdef BOARD_TDECK
    // Trackball: UP scrolls toward older messages, DOWN toward newer
    {
        TrackDir dir = trackball_poll();
        if (dir == TDIR_UP)   display_scroll(1);
        if (dir == TDIR_DOWN) display_scroll(-1);
    }

    // Touch: tap message area snaps to latest; compose area already active
    {
        int16_t tx = 0, ty = 0;
        if (touch_read(&tx, &ty)) display_touch(tx, ty);
    }

    // Keyboard input: accumulate chars, send on Enter, backspace to delete.
    // Only active when we have a working connection and are in a steady state.
    if (g_state == STATE_LORA_CONNECTED || g_state == STATE_WIFI_CONNECTED || g_state == STATE_IDLE) {
        char k = keyboard_read();
        if (k != 0) {
#ifdef GLUSTIC_FUN
            if (k == '\t') {  // Tab toggles chat ↔ map
                g_map_view = !g_map_view;
                if (!g_map_view) {
                    display_show_idle(g_kid_name, lora_last_rssi(),
                                      read_battery_pct(), queue_outbound_count());
                }
                k = 0;  // consume, don't fall through
            }
#endif
            if (k == '\r' || k == '\n') {
                // Send the message if compose buffer is non-empty
                if (g_compose_len > 0) {
                    g_compose[g_compose_len] = '\0';
                    display_push_message(g_kid_name, g_compose, true);
#ifdef TRANSPORT_P2P
                    // P2P: fire-and-forget directly; no queue, no ACK needed
                    {
                        static uint16_t s_p2p_seq = 1;
                        display_show_sending(0);
                        lora_send(g_compose, s_p2p_seq++);
                        display_refresh_status();
                    }
#elif defined(TRANSPORT_MESHTASTIC)
                    {
                        static uint16_t s_mesh_seq = 1;
                        bool mqtt_ok = wifi_mesh_mqtt_up();
                        display_show_sending(0);
                        bool lora_ok = lora_send(g_compose, s_mesh_seq++);
                        display_show_sent_path(lora_ok, mqtt_ok);
                        delay(700);
                        display_refresh_status();
                    }
#else
                    queue_push_outbound(g_compose);
                    if (wifi_up) wifi_sync_flush();
#endif
                    g_compose_len = 0;
                    g_compose[0]  = '\0';
                }
                // Clear compose bar after send
                display_show_compose("");
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
