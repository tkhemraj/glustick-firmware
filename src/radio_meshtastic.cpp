#ifdef TRANSPORT_MESHTASTIC

#include "radio_meshtastic.h"
#include "display.h"
#include <RadioLib.h>
#include <SPI.h>
#include <mbedtls/aes.h>

// ── Meshtastic LongFast PHY (US915 primary channel) ──────────────────────────
// SF9, BW250, CR4/5, preamble 16, private sync word 0x12
// Frequency is set by MESH_FREQ_MHZ build flag (default 906.875 MHz).
// To find your mesh's exact frequency: Meshtastic app → Radio config → LoRa → Frequency slot.
#define MESH_SF          9
#define MESH_BW          250.0f
#define MESH_CR          5       // CR4/5
#define MESH_PREAMBLE    16

// ── Meshtastic default public channel PSK ("LongFast", AQ== PSK = 0x01 expanded) ──
// This is the well-known Meshtastic default key. All nodes on the public mesh
// use this. Override by provisioning a custom app_key.
static const uint8_t DEFAULT_PSK[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01
};

// ── Meshtastic v2 packet header (16 bytes, transmitted unencrypted) ───────────
// Ref: https://github.com/meshtastic/firmware/blob/master/src/mesh/RadioInterface.h
struct __attribute__((packed)) MeshHeader {
    uint32_t to;        // destination node ID, LE (0xFFFFFFFF = broadcast)
    uint32_t from;      // source node ID, LE
    uint32_t id;        // packet ID, LE
    uint8_t  flags;     // hop_limit[2:0] | want_ack[3] | via_mqtt[4] | hop_start[7:5]
    uint8_t  channel;   // channel hash byte
    uint16_t next_hop;  // next-hop node for directed routing (0 = flood)
};
static_assert(sizeof(MeshHeader) == 16, "MeshHeader must be 16 bytes");

// Payload follows the header and is AES-128-CTR encrypted.
// Plaintext payload is a protobuf-encoded Data message (see below).

// ── Radio ─────────────────────────────────────────────────────────────────────
static Module s_mod(PIN_LMIC_NSS, PIN_LMIC_DIO1, PIN_LMIC_RST, LORA_BUSY_PIN);
static SX1262 s_radio(&s_mod);

static uint32_t       s_node_id    = 0;
static uint32_t       s_packet_id  = 0;
static int            s_last_rssi  = 0;
static bool           s_ready      = false;
static bool           s_tx_done    = true;
static LoRaDownlinkCb s_cb         = nullptr;
static uint8_t        s_psk[16]    = {};
static uint8_t        s_chan_hash  = 0;

static volatile bool  s_irq   = false;
static volatile bool  s_in_tx = false;

static void IRAM_ATTR on_irq() { s_irq = true; }

// ── AES-128-CTR (encrypt == decrypt) ─────────────────────────────────────────
// Nonce layout (Meshtastic spec):
//   bytes 0..7  = packet_id as 8-byte LE (top 4 bytes are 0 for 32-bit IDs)
//   bytes 8..11 = from_node as 4-byte LE
//   bytes 12..15 = 0x00
static void aes_ctr_inplace(const uint8_t *key, uint32_t from, uint32_t pkt_id,
                             uint8_t *buf, size_t len) {
    uint8_t nonce[16] = {};
    nonce[0] = pkt_id & 0xFF;
    nonce[1] = (pkt_id >>  8) & 0xFF;
    nonce[2] = (pkt_id >> 16) & 0xFF;
    nonce[3] = (pkt_id >> 24) & 0xFF;
    // bytes 4..7 = 0 (upper 32 bits of 64-bit packet ID, unused)
    nonce[8]  = from & 0xFF;
    nonce[9]  = (from >>  8) & 0xFF;
    nonce[10] = (from >> 16) & 0xFF;
    nonce[11] = (from >> 24) & 0xFF;

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, key, 128);

    uint8_t stream_block[16] = {};
    size_t  nc_off = 0;
    mbedtls_aes_crypt_ctr(&ctx, len, &nc_off, nonce, stream_block, buf, buf);
    mbedtls_aes_free(&ctx);
}

// ── Channel hash ──────────────────────────────────────────────────────────────
// Meshtastic computes a 1-byte hash over (channel name bytes + PSK bytes).
// This hash is embedded in every packet header so nodes can quickly discard
// packets on other channels without decrypting them.
static uint8_t compute_channel_hash(const char *name, const uint8_t *psk) {
    uint8_t h = 0;
    for (const char *c = name; *c; c++) h = (h + (uint8_t)*c) & 0xFF;
    for (int i = 0; i < 16; i++)        h = (h + psk[i]) & 0xFF;
    return h;
}

// ── Minimal protobuf encoder for Data{portnum=1, payload=text} ───────────────
// Meshtastic TEXT_MESSAGE_APP = portnum 1.
// Wire format:
//   0x08 0x01       — field 1 (portnum), varint, value 1
//   0x12 <len> <text> — field 2 (payload), length-delimited
static size_t encode_text(const char *text, uint8_t *out, size_t out_max) {
    size_t tlen = strlen(text);
    if (4 + tlen > out_max) tlen = out_max - 4;
    out[0] = 0x08; out[1] = 0x01;
    out[2] = 0x12; out[3] = (uint8_t)tlen;
    memcpy(out + 4, text, tlen);
    return 4 + tlen;
}

// Decode a Data protobuf — fills text_out only when portnum == 1 (text msg).
static bool decode_text(const uint8_t *buf, size_t len,
                        char *text_out, size_t text_max) {
    size_t  i        = 0;
    uint8_t portnum  = 0;

    while (i < len) {
        uint8_t tag   = buf[i++];
        uint8_t field = tag >> 3;
        uint8_t wtype = tag & 0x07;
        if (field == 1 && wtype == 0 && i < len) {
            portnum = buf[i++];
        } else if (field == 2 && wtype == 2 && i < len) {
            uint8_t plen = buf[i++];
            if (portnum == 1 && plen > 0 && plen < text_max && i + plen <= len) {
                memcpy(text_out, buf + i, plen);
                text_out[plen] = '\0';
                return true;
            }
            i += plen;
        } else {
            break; // unknown field — stop parsing
        }
    }
    return false;
}

// ── PSK parsing ───────────────────────────────────────────────────────────────
static void load_psk(const char *hex) {
    bool has_key = strlen(hex) >= 32;
    if (has_key) {
        for (int i = 0; i < 16; i++) {
            char b[3] = { hex[i * 2], hex[i * 2 + 1], '\0' };
            s_psk[i] = (uint8_t)strtoul(b, nullptr, 16);
        }
    } else {
        memcpy(s_psk, DEFAULT_PSK, 16);
    }
}

// ── Init ──────────────────────────────────────────────────────────────────────
void lora_init(const char *dev_eui_hex, const char *app_eui_hex,
               const char *app_key_hex, LoRaDownlinkCb cb) {
    s_cb = cb;

    load_psk(app_key_hex);
    s_chan_hash = compute_channel_hash("LongFast", s_psk);

    // Lower 32 bits of eFuse MAC — stable unique node ID, no init required
    uint64_t mac = ESP.getEfuseMac();
    s_node_id   = (uint32_t)(mac & 0xFFFFFFFF);
    s_packet_id = (uint32_t)esp_random();

    SPI.begin(TDECK_SPI_SCK, TDECK_SPI_MISO, TDECK_SPI_MOSI, PIN_LMIC_NSS);

    int rc = s_radio.begin(MESH_FREQ_MHZ, MESH_BW, MESH_SF, MESH_CR,
                           RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 22, MESH_PREAMBLE);
    if (rc != RADIOLIB_ERR_NONE) {
        display_show_error("Mesh radio init\nfailed");
        return;
    }

    s_radio.setDio1Action(on_irq);
    s_radio.startReceive();
    s_ready = true;
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
    size_t pkt_len = s_radio.getPacketLength();
    if (pkt_len <= sizeof(MeshHeader)) {
        s_radio.startReceive();
        return;
    }

    uint8_t buf[256];
    if (pkt_len > sizeof(buf)) {
        s_radio.startReceive();
        return;
    }

    if (s_radio.readData(buf, pkt_len) != RADIOLIB_ERR_NONE) {
        s_radio.startReceive();
        return;
    }
    s_radio.startReceive();

    MeshHeader *hdr = (MeshHeader *)buf;

    // Filter: wrong channel or not for us
    if (hdr->channel != s_chan_hash) return;
    // Header fields are LE; ESP32 is LE — direct comparison is correct
    if (hdr->to != 0xFFFFFFFF && hdr->to != s_node_id) return;

    s_last_rssi = (int)s_radio.getRSSI();

    // Decrypt payload in-place
    uint8_t *payload     = buf + sizeof(MeshHeader);
    size_t   payload_len = pkt_len - sizeof(MeshHeader);
    aes_ctr_inplace(s_psk, hdr->from, hdr->id, payload, payload_len);

    // Decode and deliver text messages
    char text[240];
    if (decode_text(payload, payload_len, text, sizeof(text))) {
        if (s_cb) s_cb(text);
    }
}

// ── Public API ────────────────────────────────────────────────────────────────
bool lora_send(const char *body, uint16_t msg_id) {
    if (!s_ready || !s_tx_done) return false;

    uint8_t buf[256];
    MeshHeader *hdr = (MeshHeader *)buf;

    hdr->to       = 0xFFFFFFFF;     // broadcast
    hdr->from     = s_node_id;
    hdr->id       = ++s_packet_id;
    // hop_start = hop_limit = 3, want_ack = 0, via_mqtt = 0
    hdr->flags    = 0x03 | (0x03 << 5);  // hop_limit[2:0]=3, hop_start[7:5]=3
    hdr->channel  = s_chan_hash;
    hdr->next_hop = 0;

    uint8_t *payload = buf + sizeof(MeshHeader);
    size_t   plen    = encode_text(body, payload, sizeof(buf) - sizeof(MeshHeader));

    aes_ctr_inplace(s_psk, s_node_id, hdr->id, payload, plen);

    s_in_tx   = true;
    s_tx_done = false;
    s_radio.startTransmit(buf, sizeof(MeshHeader) + plen);
    return true;
}

void lora_ping(uint16_t /*msg_id*/) {
    // Meshtastic uses NodeInfo packets for presence — not implemented here.
    // The mesh will route our text messages regardless of whether we announce.
}

bool lora_is_joined() { return s_ready; }
int  lora_last_rssi()  { return s_last_rssi; }

#endif // TRANSPORT_MESHTASTIC
