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
#define FLAG_VOICE 0x02   // raw Codec2 voice frame-group (len 0 = end-of-talk)
// Pairing uses a different net and these flags. A repeater must not learn
// them or the next message hop would be stuck on the pairing channel.
#define FLAG_OFFER 0xFE
#define FLAG_ACK   0xFF
#define PAIR_NET_ID 0x50414952UL

#ifndef LORA_SCK
#  define LORA_SCK  TDECK_SPI_SCK
#  define LORA_MISO TDECK_SPI_MISO
#  define LORA_MOSI TDECK_SPI_MOSI
#endif

// ── Radio ─────────────────────────────────────────────────────────────────────
static Module  s_mod(PIN_LMIC_NSS, PIN_LMIC_DIO1, PIN_LMIC_RST, LORA_BUSY_PIN);
static SX1262  s_radio(&s_mod);

static uint32_t       s_net_id    = 0;
static uint32_t       s_node_id   = 0;
static int            s_last_rssi = 0;
static bool           s_ready     = false;
static bool           s_tx_done   = true;
static LoRaDownlinkCb s_cb        = nullptr;
#ifdef FEATURE_VOICE
static LoRaVoiceCb    s_voice_cb  = nullptr;
static uint16_t       s_voice_seq = 1;
#endif

static volatile bool s_irq        = false;
static volatile bool s_in_tx      = false;
static uint32_t      s_repeats    = 0;

// Same (sender, msg id) within a minute is one packet, even if a repeater
// aired it again. Endpoints use this so a hop does not print twice.
struct Seen {
    uint32_t from;
    uint16_t id;
    uint32_t ms;
};
static Seen s_seen[16];
static int  s_seen_n = 0;

#ifdef ROLE_REPEATER
struct RepeatSlot {
    uint8_t  buf[LORA_MAX_PAYLOAD];
    uint8_t  len;
    uint32_t when;
    bool     used;
};
static RepeatSlot s_q[4];
#endif

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
static uint16_t get_u16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | p[1];
}

static bool seen_recent(uint32_t from, uint16_t id) {
    uint32_t now = millis();
    for (int i = 0; i < s_seen_n; i++) {
        if (s_seen[i].from == from && s_seen[i].id == id &&
            (uint32_t)(now - s_seen[i].ms) < 60000UL) {
            return true;
        }
    }
    return false;
}

static void remember(uint32_t from, uint16_t id) {
    uint32_t now = millis();
    int slot = 0;
    uint32_t oldest = 0xFFFFFFFFUL;
    if (s_seen_n < 16) {
        slot = s_seen_n++;
    } else {
        for (int i = 0; i < 16; i++) {
            uint32_t age = now - s_seen[i].ms;
            if (age >= oldest) { oldest = age; slot = i; }
        }
    }
    s_seen[slot].from = from;
    s_seen[slot].id   = id;
    s_seen[slot].ms   = now;
}

// ── Init ──────────────────────────────────────────────────────────────────────
void lora_init(const char *dev_eui_hex, const char *app_eui_hex,
               const char *app_key_hex, LoRaDownlinkCb cb) {
    s_cb     = cb;
    s_net_id = derive_net_id(app_key_hex);

    // Lower 32 bits of the chip eFuse MAC — unique per device, no WiFi needed
    uint64_t mac = ESP.getEfuseMac();
    s_node_id = (uint32_t)(mac & 0xFFFFFFFF);

    // Pre-configure shared SPI bus before RadioLib touches it.
    // T-Deck pins come from TDECK_SPI_*. Heltec sets LORA_SCK/MISO/MOSI.
    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, PIN_LMIC_NSS);

    // Text builds: SF10/BW125 (~1 kbps, 5-15 km). Voice builds: a faster PHY
    // (default SF8/BW125) so a voice packet's airtime stays under the audio it
    // carries — text rides the same config, trading a little range for speed.
#if defined(FEATURE_VOICE)
    float   bw = VOICE_BW_KHZ;
    uint8_t sf = VOICE_SF;
#else
    float   bw = 125.0;
    uint8_t sf = 10;
#endif
    int rc = s_radio.begin(P2P_FREQ_MHZ, bw, sf, 5,
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
#ifdef ROLE_REPEATER
static void queue_repeat(const uint8_t *buf, uint8_t len) {
    int slot = -1;
    for (int i = 0; i < 4; i++) {
        if (!s_q[i].used) { slot = i; break; }
    }
    if (slot < 0) return;
    memcpy(s_q[slot].buf, buf, len);
    s_q[slot].len  = len;
    // Stagger by node id so two Heltecs that heard the same packet
    // do not key up on the same millisecond.
    s_q[slot].when = millis() + 40UL + (s_node_id & 0x7FUL);
    s_q[slot].used = true;
}

static void pump_repeat() {
    if (!s_tx_done || s_in_tx) return;
    uint32_t now = millis();
    for (int i = 0; i < 4; i++) {
        if (!s_q[i].used || (int32_t)(now - s_q[i].when) < 0) continue;
        s_in_tx   = true;
        s_tx_done = false;
        s_radio.startTransmit(s_q[i].buf, s_q[i].len);
        s_q[i].used = false;
        s_repeats++;
        return;
    }
}
#endif

void lora_loop() {
    if (!s_ready) return;
#ifdef ROLE_REPEATER
    if (!s_irq) { pump_repeat(); return; }
#else
    if (!s_irq) return;
#endif
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

    uint32_t net     = get_u32(buf + 0);
    uint32_t from    = get_u32(buf + 4);
    uint16_t msg_id  = get_u16(buf + 8);
    uint8_t  flags   = buf[10];
    uint8_t  body_len = buf[11];

    if (from == s_node_id) return;
    if (net == PAIR_NET_ID || flags == FLAG_OFFER || flags == FLAG_ACK) return;
    if (body_len > P2P_BODY_MAX || (size_t)(P2P_HEADER + body_len) != len) return;
    if (flags != FLAG_DATA && flags != FLAG_PING
#ifdef FEATURE_VOICE
        && flags != FLAG_VOICE
#endif
       ) return;

#ifdef ROLE_REPEATER
    // Zero means "I have not heard this family's net yet."
    if (s_net_id == 0) s_net_id = net;
    if (net != s_net_id) return;
#else
    if (net != s_net_id) return;
#endif

    s_last_rssi = (int)s_radio.getRSSI();

    if (seen_recent(from, msg_id)) return;
    remember(from, msg_id);

#ifdef ROLE_REPEATER
    queue_repeat(buf, (uint8_t)len);
#else
    if (flags == FLAG_DATA && body_len > 0) {
        char body[P2P_BODY_MAX + 1];
        memcpy(body, buf + P2P_HEADER, body_len);
        body[body_len] = '\0';
        if (s_cb) s_cb(body);
    }
#ifdef FEATURE_VOICE
    else if (flags == FLAG_VOICE) {
        // Deliver even when body_len == 0 (end-of-talk marker).
        if (s_voice_cb) s_voice_cb(buf + P2P_HEADER, body_len);
    }
#endif
#endif
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
uint32_t lora_net_id() { return s_net_id; }
uint32_t lora_repeat_count() { return s_repeats; }

#ifdef FEATURE_VOICE
void lora_set_voice_cb(LoRaVoiceCb cb) { s_voice_cb = cb; }

bool lora_send_raw(const uint8_t *data, uint8_t len) {
    if (!s_ready || !s_tx_done) return false;
    if (len > P2P_BODY_MAX) len = P2P_BODY_MAX;
    send_packet(s_voice_seq++, FLAG_VOICE, (const char *)data, len);
    return true;
}
#endif

#endif // TRANSPORT_P2P
