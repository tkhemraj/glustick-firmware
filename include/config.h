#pragma once

// ── Frame protocol (must match apps/api/internal/lora/chunker.go) ────────────
#define FRAME_VERSION     1
#define FRAME_TYPE_MSG    0
#define FRAME_TYPE_ACK    1
#define FRAME_TYPE_PING   2
#define FRAME_HEADER_SIZE 6
#define LORA_MAX_PAYLOAD  50
#define CHUNK_MAX_DATA    (LORA_MAX_PAYLOAD - FRAME_HEADER_SIZE)  // 44 bytes
#define MSG_BODY_MAX      (8 * CHUNK_MAX_DATA)                    // 352 bytes — max 8-chunk message

// ── State machine ─────────────────────────────────────────────────────────────
typedef enum {
    STATE_PROVISIONING,   // First boot: BLE setup wizard
    STATE_JOINING,        // OTAA join in progress
    STATE_LORA_CONNECTED, // LoRaWAN joined, no WiFi
    STATE_WIFI_CONNECTED, // WiFi available — syncing via HTTPS
    STATE_IDLE,           // Waiting for events
    STATE_ERROR,          // Unrecoverable — show error on display
} DeviceState;

// ── NVS keys ──────────────────────────────────────────────────────────────────
#define NVS_NAMESPACE       "gsf"
#define NVS_KEY_DEV_EUI     "dev_eui"
#define NVS_KEY_APP_EUI     "app_eui"
#define NVS_KEY_APP_KEY     "app_key"
#define NVS_KEY_SERVER_URL  "server_url"
#define NVS_KEY_PARENT_TOK  "parent_tok"
#define NVS_KEY_WIFI_SSID   "wifi_ssid"
#define NVS_KEY_WIFI_PASS   "wifi_pass"
#define NVS_KEY_PROVISIONED "provisioned"
#define NVS_KEY_KID_NAME    "kid_name"
#define NVS_KEY_SERVER_CERT "srv_cert"  // PEM cert for TLS pinning (optional)

// ── Timeouts / intervals ──────────────────────────────────────────────────────
#define PING_INTERVAL_MS        (5UL * 60 * 1000)   // 5 min keepalive ping
#define WIFI_SYNC_INTERVAL_MS   (30UL * 1000)        // 30 s sync when on WiFi
#define JOIN_TIMEOUT_MS         (2UL * 60 * 1000)    // 2 min OTAA timeout
#define BLE_TIMEOUT_MS          (5UL * 60 * 1000)    // 5 min BLE advertising
#define MSG_QUEUE_MAX           32
#define MSG_ID_MAX              0xFFFF

// ── Display ───────────────────────────────────────────────────────────────────
// Heltec V3 defaults — T-Deck overrides these via build flags in platformio.ini
#ifndef DISPLAY_WIDTH
#  define DISPLAY_WIDTH   128
#endif
#ifndef DISPLAY_HEIGHT
#  define DISPLAY_HEIGHT  64
#endif

// ── T-Deck compose buffer ─────────────────────────────────────────────────────
#ifdef BOARD_TDECK
#  define COMPOSE_BUF_MAX  MSG_BODY_MAX
#endif
