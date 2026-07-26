#ifdef TRANSPORT_P2P

#include "p2p_pair.h"
#include "display.h"
#include "keyboard.h"
#include "config.h"
#include <RadioLib.h>
#include <SPI.h>
#include <Preferences.h>
#include <esp_system.h>
#include <cctype>

// Magic net-id used only during the pairing handshake — never for normal traffic
#define PAIR_NET_ID    0x50414952UL   // "PAIR"
#define FLAG_OFFER     0xFE           // HOST → broadcast: body = 32-char hex key
#define FLAG_ACK       0xFF           // JOINER → HOST: body = 6-char code confirmation
#define PAIR_HDR       12
#define CODE_LEN       6
#define KEY_HEX_LEN    32

static volatile bool s_irq   = false;
static volatile bool s_in_tx = false;
static void IRAM_ATTR pair_irq_handler() { s_irq = true; }

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static uint32_t get_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] <<  8) |  p[3];
}

// ── NVS helpers ───────────────────────────────────────────────────────────────

bool p2p_pair_is_complete() {
    Preferences p;
    p.begin(NVS_NAMESPACE, true);
    bool done = p.getBool(NVS_KEY_P2P_PAIRED, false);
    p.end();
    return done;
}

void p2p_pair_load_key(char *out_hex33) {
    Preferences p;
    p.begin(NVS_NAMESPACE, true);
    p.getString(NVS_KEY_P2P_NET_KEY, out_hex33, KEY_HEX_LEN + 1);
    p.end();
}

void p2p_pair_clear() {
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    p.remove(NVS_KEY_P2P_PAIRED);
    p.remove(NVS_KEY_P2P_NET_KEY);
    p.end();
}

static void save_paired(const char *net_key_hex) {
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    p.putString(NVS_KEY_P2P_NET_KEY, net_key_hex);
    p.putBool(NVS_KEY_P2P_PAIRED, true);
    p.end();
}

// ── Radio helpers (local instance — destroyed after pairing) ──────────────────
// lora_init() in radio_p2p.cpp creates its own instance and does a full
// hardware reset (radio.begin()), so the physical chip is re-initialised clean.

static void wait_for_irq(unsigned long timeout_ms = 5000) {
    unsigned long t0 = millis();
    while (!s_irq && millis() - t0 < timeout_ms) delay(5);
}

// ── Main pairing flow ─────────────────────────────────────────────────────────

bool p2p_pair_run() {
    SPI.begin(TDECK_SPI_SCK, TDECK_SPI_MISO, TDECK_SPI_MOSI, PIN_LMIC_NSS);

    Module  mod(PIN_LMIC_NSS, PIN_LMIC_DIO1, PIN_LMIC_RST, LORA_BUSY_PIN);
    SX1262  radio(&mod);

    if (radio.begin(P2P_FREQ_MHZ, 125.0, 7, 5,
                    RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 22, 8) != RADIOLIB_ERR_NONE) {
        display_show_error("Pair: radio init\nfailed");
        return false;
    }
    radio.setDio1Action(pair_irq_handler);

    uint64_t mac   = ESP.getEfuseMac();
    uint32_t my_id = (uint32_t)(mac & 0xFFFFFFFF);

    // ── Mode selection ────────────────────────────────────────────────────────
    display_show_p2p_mode_select();
    bool is_host   = false;
    bool got_mode  = false;
    while (!got_mode) {
        char k = keyboard_read();
        if (k == 'h' || k == 'H') { is_host = true;  got_mode = true; }
        if (k == 'j' || k == 'J') { is_host = false; got_mode = true; }
        delay(20);
    }

    // ── HOST ─────────────────────────────────────────────────────────────────
    if (is_host) {
        // Generate random 16-byte key → 32 uppercase hex chars
        uint8_t key_bytes[16];
        esp_fill_random(key_bytes, sizeof(key_bytes));
        char net_key[KEY_HEX_LEN + 1] = {};
        for (int i = 0; i < 16; i++)
            snprintf(net_key + i * 2, 3, "%02X", key_bytes[i]);

        char code[CODE_LEN + 1];
        memcpy(code, net_key, CODE_LEN);
        code[CODE_LEN] = '\0';

        display_show_p2p_host(code);

        radio.startReceive();
        s_irq   = false;
        s_in_tx = false;

        unsigned long last_offer_ms = 0;
        bool paired = false;

        while (!paired) {
            // Broadcast PAIR_OFFER every 2 s
            if (!s_in_tx && (millis() - last_offer_ms) >= 2000) {
                last_offer_ms = millis();
                uint8_t buf[PAIR_HDR + KEY_HEX_LEN];
                put_u32(buf + 0, PAIR_NET_ID);
                put_u32(buf + 4, my_id);
                buf[8] = 0; buf[9] = 0;  // msg_id unused during pairing
                buf[10] = FLAG_OFFER;
                buf[11] = KEY_HEX_LEN;
                memcpy(buf + PAIR_HDR, net_key, KEY_HEX_LEN);
                s_in_tx = true;
                s_irq   = false;
                radio.startTransmit(buf, sizeof(buf));
            }

            if (s_irq) {
                s_irq = false;
                if (s_in_tx) {
                    s_in_tx = false;
                    radio.finishTransmit();
                    radio.startReceive();
                } else {
                    size_t len = radio.getPacketLength();
                    if (len >= (size_t)(PAIR_HDR + CODE_LEN)) {
                        uint8_t buf[64] = {};
                        if (radio.readData(buf, len) == RADIOLIB_ERR_NONE
                            && get_u32(buf + 0) == PAIR_NET_ID
                            && get_u32(buf + 4) != my_id
                            && buf[10] == FLAG_ACK
                            && buf[11] >= CODE_LEN
                            && memcmp(buf + PAIR_HDR, code, CODE_LEN) == 0) {
                            save_paired(net_key);
                            paired = true;
                        }
                    }
                    if (!paired) radio.startReceive();
                }
            }
            delay(10);
        }

        display_show_p2p_paired();
        delay(1500);
        return true;
    }

    // ── JOIN ──────────────────────────────────────────────────────────────────
    char typed[CODE_LEN + 1] = {};
    int  typed_len = 0;
    display_show_p2p_join(typed, typed_len);

    radio.startReceive();
    s_irq   = false;
    s_in_tx = false;

    char pending_key[KEY_HEX_LEN + 1] = {};
    bool have_offer = false;
    bool paired     = false;

    while (!paired) {
        // Keyboard input — accept 0-9 A-F
        char k = keyboard_read();
        if (k != 0 && typed_len < CODE_LEN) {
            char ku = (char)toupper((unsigned char)k);
            if ((ku >= '0' && ku <= '9') || (ku >= 'A' && ku <= 'F')) {
                typed[typed_len++] = ku;
                typed[typed_len]   = '\0';
                display_show_p2p_join(typed, typed_len);

                if (typed_len == CODE_LEN && have_offer) {
                    // Full code entered and we already have the matching offer
                    uint8_t ack[PAIR_HDR + CODE_LEN];
                    put_u32(ack + 0, PAIR_NET_ID);
                    put_u32(ack + 4, my_id);
                    ack[8] = 0; ack[9] = 0;
                    ack[10] = FLAG_ACK;
                    ack[11] = CODE_LEN;
                    memcpy(ack + PAIR_HDR, typed, CODE_LEN);
                    radio.transmit(ack, sizeof(ack));  // blocking — one short packet
                    save_paired(pending_key);
                    paired = true;
                }
            } else if ((k == '\b' || k == 127) && typed_len > 0) {
                typed[--typed_len] = '\0';
                have_offer = false;
                display_show_p2p_join(typed, typed_len);
            }
        }

        // LoRa RX — look for PAIR_OFFER matching the code typed so far
        if (s_irq && !s_in_tx) {
            s_irq = false;
            size_t len = radio.getPacketLength();
            if (len >= (size_t)(PAIR_HDR + KEY_HEX_LEN)) {
                uint8_t buf[PAIR_HDR + KEY_HEX_LEN] = {};
                if (radio.readData(buf, len) == RADIOLIB_ERR_NONE
                    && get_u32(buf + 0) == PAIR_NET_ID
                    && buf[10] == FLAG_OFFER
                    && buf[11] == KEY_HEX_LEN) {
                    // Check first typed_len chars match
                    if (typed_len > 0 && memcmp(buf + PAIR_HDR, typed, typed_len) == 0) {
                        memcpy(pending_key, buf + PAIR_HDR, KEY_HEX_LEN);
                        pending_key[KEY_HEX_LEN] = '\0';
                        have_offer = true;

                        if (typed_len == CODE_LEN) {
                            // Got offer and full code is already typed
                            uint8_t ack[PAIR_HDR + CODE_LEN];
                            put_u32(ack + 0, PAIR_NET_ID);
                            put_u32(ack + 4, my_id);
                            ack[8] = 0; ack[9] = 0;
                            ack[10] = FLAG_ACK;
                            ack[11] = CODE_LEN;
                            memcpy(ack + PAIR_HDR, typed, CODE_LEN);
                            radio.transmit(ack, sizeof(ack));
                            save_paired(pending_key);
                            paired = true;
                        }
                    }
                }
            }
            if (!paired) radio.startReceive();
        }
        delay(10);
    }

    display_show_p2p_paired();
    delay(1500);
    return true;
}

#endif // TRANSPORT_P2P
