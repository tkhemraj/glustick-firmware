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
    uint16_t msg_id;
    uint8_t  total;
    uint8_t  received;
    char     buf[MSG_BODY_MAX + 1];
    bool     slot_received[8];  // supports up to 8 chunks
} s_reassemble;

// ── LMIC pin mapping for Heltec WiFi LoRa 32 V3 (SX1262) ────────────────────
static const lmic_pinmap lmic_pins = {
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

static void parse_hex_str(const char *hex, uint8_t *out, size_t out_len) {
    for (size_t i = 0; i < out_len; i++) {
        char byte_str[3] = { hex[i*2], hex[i*2+1], '\0' };
        out[i] = (uint8_t)strtoul(byte_str, nullptr, 16);
    }
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
    if (f.msg_id != s_reassemble.msg_id) {
        // New message — reset reassembly state
        s_reassemble.msg_id   = f.msg_id;
        s_reassemble.total    = f.chunk_total;
        s_reassemble.received = 0;
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
    parse_hex_str(dev_eui_hex, s_dev_eui, 8);
    parse_hex_str(app_eui_hex, s_app_eui, 8);
    parse_hex_str(app_key_hex, s_app_key, 16);
    s_downlink_cb = downlink_cb;

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
