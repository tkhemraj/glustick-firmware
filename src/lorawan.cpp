// Compiled only for the self-hosted LoRaWAN build.
// P2P and Meshtastic builds use radio_p2p.cpp / radio_meshtastic.cpp instead.
#if !defined(TRANSPORT_P2P) && !defined(TRANSPORT_MESHTASTIC)

#include "lorawan.h"
#include "display.h"
#include <lmic.h>
#include <hal/hal.h>
#include <SPI.h>
#include <string.h>
#include <stdlib.h>

// ── Key storage (populated by lora_init from hex strings) ────────────────────
static uint8_t s_dev_eui[8];
static uint8_t s_app_eui[8];
static uint8_t s_app_key[16];

static bool            s_joined      = false;
static bool            s_tx_busy     = false;
static int             s_last_rssi   = 0;
static LoRaDownlinkCb  s_downlink_cb = nullptr;

// Reassembly buffer for multi-chunk downlinks
static struct {
    uint16_t      msg_id;
    uint8_t       total;
    uint8_t       received;
    char          buf[MSG_BODY_MAX + 1];
    bool          slot_received[8];
    unsigned long started;  // millis() when first chunk arrived
} s_reassemble;

// ── LMIC pin mapping for Heltec WiFi LoRa 32 V3 (SX1262) ────────────────────
// Must be non-static: LMIC hal.h declares 'extern const lmic_pinmap lmic_pins'
const lmic_pinmap lmic_pins = {
    .nss    = PIN_LMIC_NSS,
    .rxtx   = PIN_LMIC_RXTX,
    .rst    = PIN_LMIC_RST,
    .dio    = { PIN_LMIC_DIO0, PIN_LMIC_DIO1, PIN_LMIC_DIO2 },
    .rxtx_rx_active = 0,
    .rssi_cal = 10,
    .spi_freq = 8000000,
};

// ── LMIC callbacks (required by library) ─────────────────────────────────────
void os_getArtEui(u1_t *buf) {
    // AppEUI is stored MSB in LoRaWAN spec, LMIC wants LSB
    for (int i = 0; i < 8; i++) buf[i] = s_app_eui[7 - i];
}

void os_getDevEui(u1_t *buf) {
    for (int i = 0; i < 8; i++) buf[i] = s_dev_eui[7 - i];
}

void os_getDevKey(u1_t *buf) {
    memcpy(buf, s_app_key, 16);
}

static bool parse_hex_str(const char *hex, uint8_t *out, size_t out_len) {
    if (strlen(hex) < out_len * 2) return false;
    for (size_t i = 0; i < out_len; i++) {
        char byte_str[3] = { hex[i*2], hex[i*2+1], '\0' };
        out[i] = (uint8_t)strtoul(byte_str, nullptr, 16);
    }
    return true;
}

static void handle_downlink(uint8_t *payload, size_t len) {
    Frame f;
    if (!frame_decode(payload, len, &f)) return;
    if (f.type != FRAME_TYPE_MSG) return;

    s_last_rssi = LMIC.rssi;
    display_set_lora_rssi(s_last_rssi);

    if (f.chunk_total == 1) {
        // Single-chunk message — deliver immediately
        char body[CHUNK_MAX_DATA + 1];
        memcpy(body, f.data, f.data_len);
        body[f.data_len] = '\0';
        if (s_downlink_cb) s_downlink_cb(body);
        return;
    }

    // Multi-chunk reassembly
    // Reset on new msg_id or if the previous reassembly stalled past the server's
    // 2-minute Redis TTL (chunks it never received won't arrive after that).
    bool stale = s_reassemble.received > 0
              && (millis() - s_reassemble.started) > 120000UL;
    if (f.msg_id != s_reassemble.msg_id || stale) {
        s_reassemble.msg_id   = f.msg_id;
        s_reassemble.total    = f.chunk_total;
        s_reassemble.received = 0;
        s_reassemble.started  = millis();
        memset(s_reassemble.buf, 0, sizeof(s_reassemble.buf));
        memset(s_reassemble.slot_received, 0, sizeof(s_reassemble.slot_received));
    }

    if (f.chunk_idx < 8 && !s_reassemble.slot_received[f.chunk_idx]) {
        size_t offset = f.chunk_idx * CHUNK_MAX_DATA;
        if (offset + f.data_len <= MSG_BODY_MAX) {
            memcpy(s_reassemble.buf + offset, f.data, f.data_len);
            s_reassemble.slot_received[f.chunk_idx] = true;
            s_reassemble.received++;
        }
    }

    if (s_reassemble.received == s_reassemble.total) {
        if (s_downlink_cb) s_downlink_cb(s_reassemble.buf);
        s_reassemble.msg_id = 0;
    }
}

void onEvent(ev_t ev) {
    switch (ev) {
        case EV_JOINED:
            s_joined  = true;
            s_tx_busy = false;
            LMIC_setLinkCheckMode(0);
            break;
        case EV_TXCOMPLETE:
            s_tx_busy = false;
            if (LMIC.dataLen > 0) {
                handle_downlink(LMIC.frame + LMIC.dataBeg, LMIC.dataLen);
            }
            break;
        case EV_JOIN_FAILED:
        case EV_REJOIN_FAILED:
            // LMIC will retry; display is updated by main state machine
            break;
        default:
            break;
    }
}

void lora_init(
    const char *dev_eui_hex,
    const char *app_eui_hex,
    const char *app_key_hex,
    LoRaDownlinkCb downlink_cb
) {
    if (!parse_hex_str(dev_eui_hex, s_dev_eui, 8) ||
        !parse_hex_str(app_eui_hex, s_app_eui, 8) ||
        !parse_hex_str(app_key_hex, s_app_key, 16)) {
        display_show_error("Bad key format\nRe-provision device");
        return;
    }
    s_downlink_cb = downlink_cb;

#ifdef BOARD_TDECK
    // T-Deck's SX1262 and ST7789 share SCK/MISO/MOSI with separate CS pins.
    // LMIC's ESP32-S3 HAL would call SPI.begin() with board defaults (GPIO
    // 11/13/12), which are wrong. Pre-initialise here so the HAL finds the
    // bus already up on the correct pins and leaves them alone.
    SPI.begin(TDECK_SPI_SCK, TDECK_SPI_MISO, TDECK_SPI_MOSI, PIN_LMIC_NSS);
#endif
    os_init_ex(&lmic_pins);
    LMIC_reset();
    // Disable link check validation (not supported by all networks)
    LMIC_setLinkCheckMode(0);
    // Set to DR_SF7 (highest speed, shortest range) — can be made configurable
    LMIC_setDrTxpow(DR_SF7, 14);
    LMIC_startJoining();

    display_show_joining();
}

void lora_loop() {
    os_runloop_once();
}

bool lora_send(const char *body, uint16_t msg_id) {
    if (!s_joined || s_tx_busy) return false;

    size_t body_len = strlen(body);
    uint8_t total_chunks = (body_len + CHUNK_MAX_DATA - 1) / CHUNK_MAX_DATA;
    if (total_chunks == 0) total_chunks = 1;

    // Send chunks one at a time; caller re-invokes for subsequent chunks
    // In practice most messages fit in one chunk (44 bytes)
    uint8_t buf[LORA_MAX_PAYLOAD];
    Frame f;
    f.version     = FRAME_VERSION;
    f.type        = FRAME_TYPE_MSG;
    f.msg_id      = msg_id;
    f.chunk_idx   = 0;
    f.chunk_total = total_chunks;
    f.data_len    = body_len < CHUNK_MAX_DATA ? body_len : CHUNK_MAX_DATA;
    memcpy(f.data, body, f.data_len);

    size_t encoded_len = frame_encode(&f, buf);
    if (LMIC.opmode & OP_TXRXPEND) return false;

    LMIC_setTxData2(1, buf, encoded_len, 0);
    s_tx_busy = true;
    return true;
}

void lora_ping(uint16_t msg_id) {
    if (!s_joined || s_tx_busy) return;
    Frame f;
    frame_make_ping(msg_id, &f);
    uint8_t buf[LORA_MAX_PAYLOAD];
    size_t len = frame_encode(&f, buf);
    LMIC_setTxData2(1, buf, len, 0);
    s_tx_busy = true;
}

bool lora_is_joined() { return s_joined; }
int  lora_last_rssi()  { return s_last_rssi; }

#endif // !TRANSPORT_P2P && !TRANSPORT_MESHTASTIC
