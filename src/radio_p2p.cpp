#ifdef TRANSPORT_P2P

#include "radio_p2p.h"
#include "display.h"
#include <RadioLib.h>
#include <SPI.h>

// ── P2P Packet layout ─────────────────────────────────────────────────────────
// NET_ID[4] | FROM_ID[4] | MSG_ID[2] | FLAGS[1] | BODY_LEN[1] | BODY[0..38]
// All multi-byte fields are big-endian.
#define P2P_HEADER  12
#define P2P_BODY_MAX (LORA_MAX_PAYLOAD - P2P_HEADER)  // 38 bytes

#define FLAG_DATA  0x00
#define FLAG_PING  0x01

// ── Radio ─────────────────────────────────────────────────────────────────────
static Module  s_mod(PIN_LMIC_NSS, PIN_LMIC_DIO1, PIN_LMIC_RST, LORA_BUSY_PIN);
static SX1262  s_radio(&s_mod);

static uint32_t       s_net_id    = 0;
static uint32_t       s_node_id   = 0;
static int            s_last_rssi = 0;
static bool           s_ready     = false;
static bool           s_tx_done   = true;
static LoRaDownlinkCb s_cb        = nullptr;

static volatile bool s_irq        = false;
static volatile bool s_in_tx      = false;

static void IRAM_ATTR on_irq() { s_irq = true; }

// ── Helpers ───────────────────────────────────────────────────────────────────
static uint32_t derive_net_id(const char *hex) {
    uint32_t id = 0;
    for (int i = 0; i < 8 && hex[i]; i++) {
        char c = hex[i];
        uint8_t n = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 0;
        id = (id << 4) | n;
    }
    return id;
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = v >> 8; p[1] = v;
}
static uint32_t get_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  | p[3];
}

// ── Init ──────────────────────────────────────────────────────────────────────
void lora_init(const char *dev_eui_hex, const char *app_eui_hex,
               const char *app_key_hex, LoRaDownlinkCb cb) {
    s_cb     = cb;
    s_net_id = derive_net_id(app_key_hex);

    // Lower 32 bits of the chip eFuse MAC — unique per device, no WiFi needed
    uint64_t mac = ESP.getEfuseMac();
    s_node_id = (uint32_t)(mac & 0xFFFFFFFF);

    // Pre-configure shared SPI bus before RadioLib touches it
    SPI.begin(TDECK_SPI_SCK, TDECK_SPI_MISO, TDECK_SPI_MOSI, PIN_LMIC_NSS);

    // SF7 BW125 CR4/5 — ~5.5 kbps, 1-5 km
    int rc = s_radio.begin(P2P_FREQ_MHZ, 125.0, 7, 5,
                           RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 22, 8);
    if (rc != RADIOLIB_ERR_NONE) {
        display_show_error("P2P radio init\nfailed");
        return;
    }
    s_radio.setDio1Action(on_irq);
    s_radio.startReceive();
    s_ready = true;
}

// ── TX helper ─────────────────────────────────────────────────────────────────
static void send_packet(uint16_t msg_id, uint8_t flags,
                        const char *body, uint8_t body_len) {
    uint8_t buf[LORA_MAX_PAYLOAD];
    put_u32(buf + 0, s_net_id);
    put_u32(buf + 4, s_node_id);
    put_u16(buf + 8, msg_id);
    buf[10] = flags;
    buf[11] = body_len;
    if (body_len) memcpy(buf + P2P_HEADER, body, body_len);

    s_in_tx   = true;
    s_tx_done = false;
    s_radio.startTransmit(buf, P2P_HEADER + body_len);
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void lora_loop() {
    if (!s_ready || !s_irq) return;
    s_irq = false;

    if (s_in_tx) {
        s_in_tx   = false;
        s_tx_done = true;
        s_radio.finishTransmit();
        s_radio.startReceive();
        return;
    }

    // RX path
    size_t len = s_radio.getPacketLength();
    if (len < P2P_HEADER || len > LORA_MAX_PAYLOAD) {
        s_radio.startReceive();
        return;
    }

    uint8_t buf[LORA_MAX_PAYLOAD];
    if (s_radio.readData(buf, len) != RADIOLIB_ERR_NONE) {
        s_radio.startReceive();
        return;
    }
    s_radio.startReceive();

    // Drop packets not on our network
    if (get_u32(buf + 0) != s_net_id) return;
    // Drop our own reflections
    if (get_u32(buf + 4) == s_node_id) return;

    s_last_rssi = (int)s_radio.getRSSI();

    uint8_t flags    = buf[10];
    uint8_t body_len = buf[11];

    if (flags == FLAG_DATA && body_len > 0) {
        if (body_len > P2P_BODY_MAX) body_len = P2P_BODY_MAX;
        char body[P2P_BODY_MAX + 1];
        memcpy(body, buf + P2P_HEADER, body_len);
        body[body_len] = '\0';
        if (s_cb) s_cb(body);
    }
}

// ── Public API ────────────────────────────────────────────────────────────────
bool lora_send(const char *body, uint16_t msg_id) {
    if (!s_ready || !s_tx_done) return false;
    size_t len = strlen(body);
    if (len > P2P_BODY_MAX) len = P2P_BODY_MAX;
    send_packet(msg_id, FLAG_DATA, body, (uint8_t)len);
    return true;
}

void lora_ping(uint16_t msg_id) {
    if (!s_ready || !s_tx_done) return;
    send_packet(msg_id, FLAG_PING, nullptr, 0);
}

bool lora_is_joined() { return s_ready; }
int  lora_last_rssi()  { return s_last_rssi; }

#endif // TRANSPORT_P2P
