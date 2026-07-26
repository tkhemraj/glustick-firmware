#ifdef TRANSPORT_MESHTASTIC

#include "wifi_mesh.h"
#include "mesh_config.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <mbedtls/aes.h>

#define WIFI_NVS_NS    "gsf_wifi"
#define WIFI_NVS_SSID  "ssid"
#define WIFI_NVS_PASS  "pass"

#define MQTT_HOST      "mqtt.meshtastic.org"
#define MQTT_PORT      1883
#define MQTT_USER      "meshdev"
#define MQTT_PASS      "large4cats"
#define MQTT_TOPIC_PFX "msh/2/e/GluStic"

#define DEDUP_SIZE          16
#define RECONNECT_MS        8000
#define ANNOUNCE_INTERVAL_MS (15UL * 60UL * 1000UL)

// Receive queue — wifi_mesh_task pushes here; wifi_mesh_loop() drains on loopTask
#define RX_QUEUE_SIZE 4
static char          s_rx_queue[RX_QUEUE_SIZE][200];
static volatile int  s_rx_head = 0;
static volatile int  s_rx_tail = 0;

static uint32_t     s_node_id  = 0;
static uint8_t      s_psk[16]  = {};
static char         s_channel[32] = {};
static WifiMeshRxCb s_rx_cb    = nullptr;

static char         s_gateway_id[12]    = {};  // "!a1b2c3d4"
static char         s_topic_tx[64]      = {};  // GluStic publish topic
static char         s_topic_sub[64]     = {};  // GluStic subscribe topic
static char         s_topic_longfast[64]= {};  // LongFast shadow-publish topic

static WiFiClient   s_wifi_client;
static PubSubClient s_mqtt(s_wifi_client);

static uint32_t     s_dedup[DEDUP_SIZE] = {};
static int          s_dedup_head        = 0;
static unsigned long s_last_reconnect   = 0;
static unsigned long s_last_announce_ms = 0;
static uint32_t     s_lf_pkt_id        = 0xCC000001;

// ── AES-128-CTR ───────────────────────────────────────────────────────────────
static void aes_ctr_inplace(const uint8_t *key, uint32_t from_id, uint32_t pkt_id,
                              uint8_t *buf, size_t len) {
    uint8_t nonce[16] = {};
    nonce[0] = pkt_id & 0xFF;
    nonce[1] = (pkt_id >>  8) & 0xFF;
    nonce[2] = (pkt_id >> 16) & 0xFF;
    nonce[3] = (pkt_id >> 24) & 0xFF;
    nonce[8]  = from_id & 0xFF;
    nonce[9]  = (from_id >>  8) & 0xFF;
    nonce[10] = (from_id >> 16) & 0xFF;
    nonce[11] = (from_id >> 24) & 0xFF;

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, key, 128);
    uint8_t stream_block[16] = {};
    size_t nc_off = 0;
    mbedtls_aes_crypt_ctr(&ctx, len, &nc_off, nonce, stream_block, buf, buf);
    mbedtls_aes_free(&ctx);
}

// ── Minimal protobuf encode ───────────────────────────────────────────────────
static size_t put_varint(uint8_t *out, uint64_t v) {
    size_t n = 0;
    do {
        uint8_t b = v & 0x7F;
        v >>= 7;
        if (v) b |= 0x80;
        out[n++] = b;
    } while (v);
    return n;
}
static size_t pb_varint(uint8_t *out, uint8_t field, uint64_t val) {
    size_t n = put_varint(out, ((uint64_t)field << 3) | 0);
    n += put_varint(out + n, val);
    return n;
}
static size_t pb_bytes(uint8_t *out, uint8_t field, const uint8_t *data, size_t dlen) {
    size_t n = put_varint(out, ((uint64_t)field << 3) | 2);
    n += put_varint(out + n, dlen);
    memcpy(out + n, data, dlen);
    return n + dlen;
}
static size_t pb_str(uint8_t *out, uint8_t field, const char *s) {
    return pb_bytes(out, field, (const uint8_t *)s, strlen(s));
}

// Encode MeshPacket: to=1, from=3, id=6, encrypted=8, hop_limit=10
static size_t encode_mesh_packet(uint8_t *out,
                                  uint32_t to, uint32_t from, uint32_t id,
                                  const uint8_t *enc, size_t elen,
                                  uint8_t hop_limit) {
    uint8_t *p = out;
    p += pb_varint(p, 1, to);
    p += pb_varint(p, 3, from);
    p += pb_varint(p, 6, id);
    p += pb_bytes(p, 8, enc, elen);
    p += pb_varint(p, 10, hop_limit);
    return p - out;
}

// Encode ServiceEnvelope: packet=1, channel_id=2, gateway_id=3
static size_t encode_service_envelope(uint8_t *out,
                                       const uint8_t *pkt, size_t plen,
                                       const char *chan, const char *gw) {
    uint8_t *p = out;
    p += pb_bytes(p, 1, pkt, plen);
    p += pb_str(p, 2, chan);
    p += pb_str(p, 3, gw);
    return p - out;
}

// Encode NodeInfo Data message: portnum=4, payload=User{id=1,long=2,short=3}
static size_t encode_nodeinfo(uint8_t *out, const char *node_id_str,
                               const char *long_name, const char *short_name) {
    uint8_t user[128]; uint8_t *u = user;
    u += pb_str(u, 1, node_id_str);
    u += pb_str(u, 2, long_name);
    u += pb_str(u, 3, short_name);
    uint8_t *p = out;
    p += pb_varint(p, 1, 4);               // portnum = NODEINFO_APP
    p += pb_bytes(p, 2, user, u - user);   // payload = encoded User
    return p - out;
}

// ── Minimal protobuf decode ───────────────────────────────────────────────────
static uint64_t pb_read_varint(const uint8_t *buf, size_t len, size_t *pos) {
    uint64_t v = 0; int shift = 0;
    while (*pos < len) {
        uint8_t b = buf[(*pos)++];
        v |= (uint64_t)(b & 0x7F) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
    }
    return v;
}
static void pb_skip(const uint8_t *buf, size_t len, size_t *pos, uint8_t wire) {
    if (wire == 0) { pb_read_varint(buf, len, pos); }
    else if (wire == 2) { size_t n = (size_t)pb_read_varint(buf, len, pos); *pos += n; }
    else if (wire == 5) { *pos += 4; }
    else if (wire == 1) { *pos += 8; }
}
static bool pb_find_varint32(const uint8_t *buf, size_t len,
                              uint8_t want, uint32_t *out) {
    size_t pos = 0;
    while (pos < len) {
        uint32_t tag = (uint32_t)pb_read_varint(buf, len, &pos);
        if (tag == 0) break;
        uint8_t field = tag >> 3, wire = tag & 7;
        if (field == want && wire == 0) { *out = (uint32_t)pb_read_varint(buf, len, &pos); return true; }
        pb_skip(buf, len, &pos, wire);
    }
    return false;
}
static const uint8_t *pb_find_bytes_field(const uint8_t *buf, size_t len,
                                            uint8_t want, size_t *out_len) {
    size_t pos = 0;
    while (pos < len) {
        uint32_t tag = (uint32_t)pb_read_varint(buf, len, &pos);
        if (tag == 0) break;
        uint8_t field = tag >> 3, wire = tag & 7;
        if (field == want && wire == 2) {
            *out_len = (size_t)pb_read_varint(buf, len, &pos);
            return buf + pos;
        }
        pb_skip(buf, len, &pos, wire);
    }
    return nullptr;
}

// Decode Data protobuf → text if portnum==1 (TEXT_MESSAGE_APP)
static bool decode_text_data(const uint8_t *buf, size_t len,
                               char *text_out, size_t text_max) {
    uint32_t portnum = 0;
    size_t   plen    = 0;
    const uint8_t *payload = nullptr;
    size_t pos = 0;
    while (pos < len) {
        uint32_t tag = (uint32_t)pb_read_varint(buf, len, &pos);
        if (tag == 0) break;
        uint8_t field = tag >> 3, wire = tag & 7;
        if (field == 1 && wire == 0) {
            portnum = (uint32_t)pb_read_varint(buf, len, &pos);
        } else if (field == 2 && wire == 2) {
            plen    = (size_t)pb_read_varint(buf, len, &pos);
            payload = buf + pos;
            pos    += plen;
        } else {
            pb_skip(buf, len, &pos, wire);
        }
    }
    if (portnum == 1 && payload && plen > 0 && plen < text_max) {
        memcpy(text_out, payload, plen);
        text_out[plen] = '\0';
        return true;
    }
    return false;
}

// ── MQTT message callback ─────────────────────────────────────────────────────
static void on_mqtt_message(char * /*topic*/, uint8_t *payload, unsigned int length) {
    if (length < 4 || length > 512) return;

    // ServiceEnvelope → field 1 = MeshPacket
    size_t mp_len = 0;
    const uint8_t *mp = pb_find_bytes_field(payload, length, 1, &mp_len);
    if (!mp || mp_len < 8) return;

    uint32_t to = 0, from = 0, pkt_id = 0;
    pb_find_varint32(mp, mp_len, 1, &to);
    pb_find_varint32(mp, mp_len, 3, &from);
    pb_find_varint32(mp, mp_len, 6, &pkt_id);

    // Ignore wrong destination and our own echoed TX
    if (to != 0xFFFFFFFF && to != s_node_id) return;
    if (from == s_node_id) return;

    // Dedup: skip if already received over LoRa
    if (wifi_mesh_dedup(pkt_id)) return;

    size_t enc_len = 0;
    const uint8_t *enc = pb_find_bytes_field(mp, mp_len, 8, &enc_len);
    if (!enc || enc_len == 0 || enc_len > 240) return;

    static uint8_t plain[240];
    memcpy(plain, enc, enc_len);
    aes_ctr_inplace(s_psk, from, pkt_id, plain, enc_len);

    static char text[200];
    if (decode_text_data(plain, enc_len, text, sizeof(text))) {
        // Push to queue — drained by wifi_mesh_loop() on loopTask (safe for display)
        int next = (s_rx_head + 1) % RX_QUEUE_SIZE;
        if (next != s_rx_tail) {
            strncpy(s_rx_queue[s_rx_head], text, 199);
            s_rx_queue[s_rx_head][199] = '\0';
            s_rx_head = next;
        }
    }
}

// ── LongFast shadow-publish ───────────────────────────────────────────────────
// wmesh task (core 0) only — used by mqtt_connect_once & publish_longfast_announce.
// Never touched by loopTask (core 1).
static uint8_t s_pb_data[128];
static uint8_t s_pb_mp[260];   // 260 = worst-case header(12) + max-payload(240) + tag/len(8)
static uint8_t s_pb_env[320];
static char    s_pb_long_name[32];
static char    s_pb_short_name[8];

// ── TX queue: loopTask → wmesh task (MQTT publish) ───────────────────────────
// wifi_mesh_send() is called from loopTask (core 1).  All s_mqtt calls must
// happen on the wmesh task (core 0) because PubSubClient is not thread-safe.
// Encode the envelope on loopTask into this queue; wmesh task drains it.
#define TX_QUEUE_SIZE 4
struct TxEntry {
    uint8_t      env[320];
    unsigned int env_len;
    char         topic[72];   // matches s_topic_tx length
};
static TxEntry      s_tx_queue[TX_QUEUE_SIZE];
static volatile int s_tx_head = 0;
static volatile int s_tx_tail = 0;

// Separate encode scratch for wifi_mesh_send (loopTask, core 1).
static uint8_t s_tx_mp[260];
static uint8_t s_tx_env[320];

static void publish_longfast_announce() {
    if (!s_mqtt.connected()) return;

    snprintf(s_pb_long_name,  sizeof(s_pb_long_name),  "GluStic %s", s_gateway_id + 1);
    snprintf(s_pb_short_name, sizeof(s_pb_short_name), "GS%02X", (uint8_t)(s_node_id & 0xFF));

    size_t data_len = encode_nodeinfo(s_pb_data, s_gateway_id,
                                       s_pb_long_name, s_pb_short_name);
    aes_ctr_inplace(LONGFAST_PSK, s_node_id, s_lf_pkt_id, s_pb_data, data_len);

    size_t mp_len  = encode_mesh_packet(s_pb_mp, 0xFFFFFFFF, s_node_id, s_lf_pkt_id,
                                         s_pb_data, data_len, 3);
    size_t env_len = encode_service_envelope(s_pb_env, s_pb_mp, mp_len,
                                              LONGFAST_CHANNEL, s_gateway_id);
    s_mqtt.publish(s_topic_longfast, s_pb_env, (unsigned int)env_len, false);
    s_lf_pkt_id++;
    s_last_announce_ms = millis();
}

// ── MQTT connect + NodeInfo publish ──────────────────────────────────────────
static void mqtt_connect_once() {
    static char client_id[28];
    snprintf(client_id, sizeof(client_id), "gs_%08lx", (unsigned long)s_node_id);

    if (!s_mqtt.connect(client_id, MQTT_USER, MQTT_PASS)) return;

    s_mqtt.subscribe(s_topic_sub);

    snprintf(s_pb_long_name,  sizeof(s_pb_long_name),  "GluStic %s", s_gateway_id + 1);
    snprintf(s_pb_short_name, sizeof(s_pb_short_name), "GS%02X", (uint8_t)(s_node_id & 0xFF));

    size_t  data_len = encode_nodeinfo(s_pb_data, s_gateway_id,
                                        s_pb_long_name, s_pb_short_name);
    static uint32_t ni_pkt_id = 0xBB000001;
    aes_ctr_inplace(s_psk, s_node_id, ni_pkt_id, s_pb_data, data_len);

    size_t mp_len  = encode_mesh_packet(s_pb_mp, 0xFFFFFFFF, s_node_id, ni_pkt_id,
                                         s_pb_data, data_len, 3);
    size_t env_len = encode_service_envelope(s_pb_env, s_pb_mp, mp_len,
                                              s_channel, s_gateway_id);
    s_mqtt.publish(s_topic_tx, s_pb_env, (unsigned int)env_len, false);

    // Also announce on LongFast so we appear on the public Meshtastic map
    publish_longfast_announce();
}

// ── Public API ────────────────────────────────────────────────────────────────
void wifi_mesh_init(uint32_t node_id, const uint8_t psk[16],
                    const char *channel, uint8_t /*chan_hash*/,
                    WifiMeshRxCb cb) {
    s_node_id = node_id;
    memcpy(s_psk, psk, 16);
    strncpy(s_channel, channel, sizeof(s_channel) - 1);
    s_rx_cb   = cb;

    snprintf(s_gateway_id,    sizeof(s_gateway_id),    "!%08lx",          (unsigned long)node_id);
    snprintf(s_topic_tx,      sizeof(s_topic_tx),      "%s/%s",           MQTT_TOPIC_PFX, s_gateway_id);
    snprintf(s_topic_sub,     sizeof(s_topic_sub),      "%s/#",            MQTT_TOPIC_PFX);
    snprintf(s_topic_longfast, sizeof(s_topic_longfast), "msh/2/e/LongFast/%s", s_gateway_id);

    s_mqtt.setServer(MQTT_HOST, MQTT_PORT);
    s_mqtt.setCallback(on_mqtt_message);
    s_mqtt.setBufferSize(512);
}

bool wifi_mesh_load_creds(char *ssid_out, char *pass_out,
                           size_t ssid_max, size_t pass_max) {
    Preferences p;
    p.begin(WIFI_NVS_NS, true);
    String ssid = p.getString(WIFI_NVS_SSID, "");
    String pass = p.getString(WIFI_NVS_PASS, "");
    p.end();
    if (ssid.length() == 0) return false;
    strncpy(ssid_out, ssid.c_str(), ssid_max - 1); ssid_out[ssid_max - 1] = '\0';
    strncpy(pass_out, pass.c_str(), pass_max - 1); pass_out[pass_max - 1] = '\0';
    return true;
}

void wifi_mesh_save_creds(const char *ssid, const char *pass) {
    Preferences p;
    p.begin(WIFI_NVS_NS, false);
    p.putString(WIFI_NVS_SSID, ssid);
    p.putString(WIFI_NVS_PASS, pass ? pass : "");
    p.end();
}

// ── Background WiFi+MQTT init task ────────────────────────────────────────────
// WiFi RF calibration on first boot requires ~10-12 KB of stack.  The default
// Arduino loopTask only gets 8 KB, so we run init in its own 20 KB task.
static volatile bool s_init_done = false;
static char          s_init_ssid[64] = {};
static char          s_init_pass[64] = {};

// Permanent wifi+MQTT task — lives for the device lifetime.
// All TCP/MQTT stack frames stay here (20KB), never on loopTask (8KB).
static void wifi_mesh_task_fn(void * /*arg*/) {
    Serial.printf("[wifi] task: connecting to '%s'\n", s_init_ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(s_init_ssid, s_init_pass);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
        delay(200);
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[wifi] task: up, rssi=%d\n", (int)WiFi.RSSI());
        delay(500);  // let DHCP / DNS settle
        mqtt_connect_once();
        Serial.printf("[mqtt] task: connected=%d state=%d\n",
                      (int)s_mqtt.connected(), s_mqtt.state());
    } else {
        Serial.println("[wifi] task: connect FAILED");
    }

    s_last_reconnect = millis();
    s_init_done = true;

    // Permanent service loop — handles MQTT keepalive + auto-reconnect
    while (true) {
        if (WiFi.status() != WL_CONNECTED) {
            delay(5000);
            WiFi.reconnect();
        } else if (!s_mqtt.connected()) {
            unsigned long now = millis();
            if (now - s_last_reconnect >= RECONNECT_MS) {
                s_last_reconnect = now;
                mqtt_connect_once();
            }
            delay(100);
        } else {
            s_mqtt.loop();

            // Drain loopTask TX queue — safe to call s_mqtt.publish() here
            while (s_tx_tail != s_tx_head) {
                TxEntry *e = &s_tx_queue[s_tx_tail];
                if (e->env_len > 0) {
                    s_mqtt.publish(e->topic, e->env, e->env_len, false);
                }
                s_tx_tail = (s_tx_tail + 1) % TX_QUEUE_SIZE;
            }

            // Periodic LongFast re-announce
            if (millis() - s_last_announce_ms >= ANNOUNCE_INTERVAL_MS) {
                publish_longfast_announce();
            }
            delay(10);
        }
    }
}

void wifi_mesh_begin(const char *ssid, const char *pass) {
    strncpy(s_init_ssid, ssid, sizeof(s_init_ssid) - 1);
    strncpy(s_init_pass, pass, sizeof(s_init_pass) - 1);
    s_init_done = false;
    xTaskCreatePinnedToCore(wifi_mesh_task_fn, "wmesh", 20480,
                             nullptr, 1, nullptr, 0);  // pin to core 0
}

bool wifi_mesh_ready() { return s_init_done; }

void wifi_mesh_loop() {
    // Drain the receive queue — safe to call display functions here (loopTask / core 1)
    while (s_rx_tail != s_rx_head) {
        if (s_rx_cb) s_rx_cb(s_rx_queue[s_rx_tail]);
        s_rx_tail = (s_rx_tail + 1) % RX_QUEUE_SIZE;
    }
}

void wifi_mesh_send(uint32_t to, uint32_t from, uint32_t pkt_id,
                     const uint8_t *encrypted, size_t elen, uint8_t hop_limit) {
    if (!s_mqtt.connected()) return;
    if (elen > 240) return;  // guard against oversized payloads

    // Encode on loopTask (core 1) using dedicated TX scratch buffers.
    // Push the encoded envelope to the TX queue; wmesh task (core 0) drains it
    // and calls s_mqtt.publish().  PubSubClient is NOT thread-safe so all
    // publish calls must happen on the wmesh task.
    int next = (s_tx_head + 1) % TX_QUEUE_SIZE;
    if (next == s_tx_tail) return;  // queue full — drop

    TxEntry *e = &s_tx_queue[s_tx_head];
    size_t mp_len  = encode_mesh_packet(s_tx_mp, to, from, pkt_id, encrypted, elen, hop_limit);
    e->env_len     = (unsigned int)encode_service_envelope(s_tx_env, s_tx_mp, mp_len,
                                                           s_channel, s_gateway_id);
    if (e->env_len > sizeof(e->env)) { e->env_len = 0; return; }
    memcpy(e->env, s_tx_env, e->env_len);
    strncpy(e->topic, s_topic_tx, sizeof(e->topic) - 1);
    e->topic[sizeof(e->topic) - 1] = '\0';
    s_tx_head = next;
}

// ── Position publish (LongFast, portnum 3) ────────────────────────────────────
static uint32_t zigzag32(int32_t n) {
    return ((uint32_t)n << 1) ^ (uint32_t)(n >> 31);
}

void wifi_mesh_publish_position(float lat, float lon) {
    if (!s_mqtt.connected()) return;
    if (lat == 0.0f && lon == 0.0f) return;

    // Encode Position into s_pb_data directly
    int32_t lat_i = (int32_t)(lat * 1e7f);
    int32_t lon_i = (int32_t)(lon * 1e7f);
    uint8_t pos[20]; uint8_t *pp = pos;
    pp += put_varint(pp, (1u << 3) | 0); pp += put_varint(pp, zigzag32(lat_i));
    pp += put_varint(pp, (2u << 3) | 0); pp += put_varint(pp, zigzag32(lon_i));

    uint8_t *dp = s_pb_data;
    dp += pb_varint(dp, 1, 3);
    dp += pb_bytes(dp, 2, pos, pp - pos);
    size_t data_len = dp - s_pb_data;

    aes_ctr_inplace(LONGFAST_PSK, s_node_id, s_lf_pkt_id, s_pb_data, data_len);

    size_t mp_len  = encode_mesh_packet(s_pb_mp, 0xFFFFFFFF, s_node_id, s_lf_pkt_id,
                                         s_pb_data, data_len, 3);
    size_t env_len = encode_service_envelope(s_pb_env, s_pb_mp, mp_len,
                                              LONGFAST_CHANNEL, s_gateway_id);
    s_mqtt.publish(s_topic_longfast, s_pb_env, (unsigned int)env_len, false);
    s_lf_pkt_id++;
}

bool wifi_mesh_dedup(uint32_t pkt_id) {
    if (pkt_id == 0) return false;
    for (int i = 0; i < DEDUP_SIZE; i++) {
        if (s_dedup[i] == pkt_id) return true;
    }
    s_dedup[s_dedup_head] = pkt_id;
    s_dedup_head = (s_dedup_head + 1) % DEDUP_SIZE;
    return false;
}

bool wifi_mesh_wifi_up() { return WiFi.status() == WL_CONNECTED; }
bool wifi_mesh_mqtt_up() { return s_mqtt.connected(); }
int  wifi_mesh_rssi()    { return wifi_mesh_wifi_up() ? (int)WiFi.RSSI() : 0; }

#endif // TRANSPORT_MESHTASTIC
