#include "display.h"
#include "config.h"

// ═════════════════════════════════════════════════════════════════════════════
// T-Deck — ST7789 320×240 colour TFT via TFT_eSPI
// ═════════════════════════════════════════════════════════════════════════════
#ifdef BOARD_TDECK

#include <TFT_eSPI.h>
#include <SPI.h>
#include <qrcode.h>

static TFT_eSPI tft;

// ── Palette ───────────────────────────────────────────────────────────────────
#define C_BG        0x0D0Du   // deep navy  (#0D0D1A)
#define C_HDR       0x1082u   // dark slate (#102244 approx)
#define C_ACCENT    0x07E0u   // green      (TFT_GREEN)
#define C_TEXT      TFT_WHITE
#define C_DIM       0x8410u   // mid-grey
#define C_WARN      0xFC00u   // amber
#define C_ERR       0xF800u   // red        (TFT_RED)
#define C_BUBBLE_IN  0x1082u  // incoming message bubble
#define C_BUBBLE_OUT 0x0421u  // outgoing message bubble (dark green-ish)

static const uint16_t W = DISPLAY_WIDTH;   // 320
static const uint16_t H = DISPLAY_HEIGHT;  // 240

// ── Header bar (top 28 px) ────────────────────────────────────────────────────
static void draw_header(const char *title, const char *right_label = nullptr) {
    tft.fillRect(0, 0, W, 28, C_HDR);
    tft.setTextColor(C_ACCENT, C_HDR);
    tft.setTextFont(2);  // 16px
    tft.setCursor(6, 6);
    tft.print(title);
    if (right_label) {
        tft.setTextColor(C_DIM, C_HDR);
        tft.setTextFont(1);  // 8px
        int x = W - strlen(right_label) * 6 - 4;
        tft.setCursor(x, 10);
        tft.print(right_label);
    }
    tft.drawFastHLine(0, 28, W, C_ACCENT);
}

// ── Battery bar (bottom 18 px) ────────────────────────────────────────────────
static void draw_battery_bar(int pct, bool wifi, bool lora) {
    tft.fillRect(0, H - 18, W, 18, C_HDR);
    tft.drawFastHLine(0, H - 18, W, C_DIM);

    // Battery pill on the right
    uint16_t bat_col = pct > 25 ? C_ACCENT : (pct > 10 ? C_WARN : C_ERR);
    tft.drawRect(W - 38, H - 14, 32, 10, C_DIM);
    tft.fillRect(W - 36, H - 12, (pct * 28) / 100, 6, bat_col);
    tft.fillRect(W - 6, H - 11, 3, 4, C_DIM);  // nub

    // Radio indicators on left
    tft.setTextFont(1);
    tft.setCursor(4, H - 13);
    tft.setTextColor(wifi ? C_ACCENT : C_DIM, C_HDR);
    tft.print("WiFi");
    tft.setTextColor(C_DIM, C_HDR);
    tft.print(" | ");
    tft.setTextColor(lora ? C_ACCENT : C_DIM, C_HDR);
    tft.print("LoRa");
}

void display_init() {
    // T-Deck power-on latch — must be HIGH or the board powers off
    pinMode(TDECK_POWERON_PIN, OUTPUT);
    digitalWrite(TDECK_POWERON_PIN, HIGH);

    tft.init();
    tft.setRotation(1);  // landscape, keyboard at the bottom
    tft.fillScreen(C_BG);

    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
}

void display_show_boot() {
    tft.fillScreen(C_BG);
    tft.setTextColor(C_ACCENT, C_BG);
    tft.setTextFont(4);  // 26px
    tft.setCursor(60, 80);
    tft.print("Glustick");
    tft.setTextFont(2);
    tft.setTextColor(C_DIM, C_BG);
    tft.setCursor(128, 114);
    tft.print("Link");
    tft.setTextFont(1);
    tft.setCursor(134, 136);
    tft.print(FW_VERSION);
}

void display_show_provisioning(const char *ble_name) {
    tft.fillScreen(C_BG);
    draw_header("SETUP");
    tft.setTextColor(C_TEXT, C_BG);
    tft.setTextFont(2);
    tft.setCursor(20, 50);
    tft.print("Open the Glustick app");
    tft.setCursor(20, 74);
    tft.print("and tap Add device.");
    tft.setTextColor(C_DIM, C_BG);
    tft.setTextFont(1);
    tft.setCursor(20, 110);
    tft.print("BLE: ");
    tft.setTextColor(C_ACCENT, C_BG);
    tft.print(ble_name);
}

void display_show_joining() {
    tft.fillScreen(C_BG);
    draw_header("CONNECTING");
    tft.setTextColor(C_TEXT, C_BG);
    tft.setTextFont(2);
    tft.setCursor(20, 60);
    tft.print("Joining LoRaWAN...");
    tft.setTextFont(1);
    tft.setTextColor(C_DIM, C_BG);
    tft.setCursor(20, 92);
    tft.print("This can take up to 2 minutes.");
}

void display_show_idle(const char *kid_name, int rssi, int battery_pct, uint8_t pending) {
    tft.fillScreen(C_BG);
    draw_header("GLUSTICK LINK");
    draw_battery_bar(battery_pct, false, rssi != 0);

    // Kid name — large
    tft.setTextFont(4);
    tft.setTextColor(C_TEXT, C_BG);
    tft.setCursor(20, 50);
    tft.print(kid_name);

    // RSSI
    char rssi_buf[16];
    snprintf(rssi_buf, sizeof(rssi_buf), "%d dBm", rssi);
    tft.setTextFont(1);
    tft.setTextColor(C_DIM, C_BG);
    tft.setCursor(20, 90);
    tft.print(rssi_buf);

    // Pending badge
    if (pending > 0) {
        tft.fillRoundRect(20, 108, 100, 22, 4, C_WARN);
        tft.setTextColor(TFT_BLACK, C_WARN);
        tft.setTextFont(2);
        char pend_buf[20];
        snprintf(pend_buf, sizeof(pend_buf), "  %u pending", pending);
        tft.setCursor(24, 112);
        tft.print(pend_buf);
    } else {
        tft.setTextColor(C_DIM, C_BG);
        tft.setTextFont(1);
        tft.setCursor(20, 120);
        tft.print("Type a message and press Enter");
    }
}

void display_show_message(const char *from, const char *body) {
    tft.fillScreen(C_BG);
    draw_header("MESSAGE");

    // Sender label
    tft.setTextFont(2);
    tft.setTextColor(C_ACCENT, C_BG);
    tft.setCursor(10, 38);
    tft.print(from);

    // Message bubble
    tft.fillRoundRect(8, 60, W - 16, H - 90, 6, C_BUBBLE_IN);
    tft.setTextColor(C_TEXT, C_BUBBLE_IN);
    tft.setTextFont(2);

    // Word-wrap at ~46 chars per line in Font2 (6px wide + gap)
    const int LINE_CHARS = 46;
    int body_len = strlen(body);
    int y = 68;
    for (int i = 0; i < body_len && y < H - 38; i += LINE_CHARS) {
        char line[47];
        strncpy(line, body + i, LINE_CHARS);
        line[LINE_CHARS] = '\0';
        tft.setCursor(14, y);
        tft.print(line);
        y += 18;
    }

    tft.setTextFont(1);
    tft.setTextColor(C_DIM, C_BG);
    tft.setCursor(10, H - 26);
    tft.print("Press any key to dismiss");
}

void display_show_compose(const char *draft) {
    // Redraw only the compose strip at the bottom — avoids full-screen flicker
    int strip_y = H - 46;
    tft.fillRect(0, strip_y, W, 28, C_BUBBLE_OUT);
    tft.drawFastHLine(0, strip_y, W, C_ACCENT);
    tft.setTextColor(C_TEXT, C_BUBBLE_OUT);
    tft.setTextFont(2);
    tft.setCursor(6, strip_y + 6);

    // Show last 46 chars of draft so cursor is always visible
    int dlen = strlen(draft);
    const char *show = dlen > 46 ? draft + dlen - 46 : draft;
    tft.print(show);

    // Blinking cursor block
    tft.fillRect(6 + strlen(show) * 7, strip_y + 6, 7, 14, C_ACCENT);
}

void display_show_sending(uint8_t pending) {
    tft.fillScreen(C_BG);
    draw_header("SENDING");
    tft.setTextFont(2);
    tft.setTextColor(C_TEXT, C_BG);
    tft.setCursor(20, 60);
    char buf[40];
    snprintf(buf, sizeof(buf), "%u message(s) queued", pending);
    tft.print(buf);
    tft.setTextColor(C_DIM, C_BG);
    tft.setTextFont(1);
    tft.setCursor(20, 86);
    tft.print("Transmitting over LoRa...");
}

void display_show_wifi_sync() {
    tft.fillScreen(C_BG);
    draw_header("WIFI SYNC");
    tft.setTextFont(2);
    tft.setTextColor(C_ACCENT, C_BG);
    tft.setCursor(20, 60);
    tft.print("WiFi connected.");
    tft.setTextColor(C_DIM, C_BG);
    tft.setTextFont(1);
    tft.setCursor(20, 86);
    tft.print("Syncing messages with server...");
}

void display_show_error(const char *msg) {
    tft.fillScreen(C_BG);
    tft.fillRect(0, 0, W, 28, C_ERR);
    tft.setTextColor(TFT_WHITE, C_ERR);
    tft.setTextFont(2);
    tft.setCursor(6, 6);
    tft.print("ERROR");

    tft.setTextColor(C_TEXT, C_BG);
    tft.setTextFont(1);
    const int LINE_CHARS = 52;
    int len = strlen(msg), y = 40;
    for (int i = 0; i < len && y < H - 10; i += LINE_CHARS) {
        char line[53];
        strncpy(line, msg + i, LINE_CHARS);
        line[LINE_CHARS] = '\0';
        tft.setCursor(8, y);
        tft.print(line);
        y += 12;
    }
}

void display_set_lora_rssi(int /*rssi*/) {}  // updated on next idle refresh

void display_show_provisioning_qr(
    const char *dev_eui, const char *app_eui, const char *app_key,
    const char *ble_name, uint32_t pin_code
) {
    tft.fillScreen(C_BG);
    draw_header("SCAN TO PROVISION");

    // QR payload: "GSF:{devEUI}:{appEUI}:{appKey}" — 70 chars, version 3 ECC_LOW
    char qr_data[72];
    snprintf(qr_data, sizeof(qr_data), "GSF:%s:%s:%s", dev_eui, app_eui, app_key);

    QRCode qrcode;
    uint8_t qr_buf[qrcode_getBufferSize(3)];
    qrcode_initText(&qrcode, qr_buf, 3, ECC_LOW, qr_data);

    // Draw QR on the left: 29 modules × 5px = 145px, centred in 160px panel
    const int QR_PX  = 5;
    const int QR_OFF = (160 - qrcode.size * QR_PX) / 2;
    tft.fillRect(0, 30, 160, H - 30, TFT_WHITE);
    for (uint8_t y = 0; y < qrcode.size; y++) {
        for (uint8_t x = 0; x < qrcode.size; x++) {
            uint16_t col = qrcode_getModule(&qrcode, x, y) ? TFT_BLACK : TFT_WHITE;
            tft.fillRect(QR_OFF + x * QR_PX, 34 + y * QR_PX, QR_PX, QR_PX, col);
        }
    }

    // Right panel: BLE name + PIN
    tft.setTextColor(C_DIM, C_BG);
    tft.setTextFont(1);
    tft.setCursor(168, 40);
    tft.print("BLE NAME");

    tft.setTextColor(C_TEXT, C_BG);
    tft.setTextFont(2);
    const char *suffix = strrchr(ble_name, '-');
    tft.setCursor(168, 54);
    tft.print(suffix ? suffix + 1 : ble_name);

    tft.setTextColor(C_DIM, C_BG);
    tft.setTextFont(1);
    tft.setCursor(168, 92);
    tft.print("BLE PIN");

    char pin_buf[8];
    snprintf(pin_buf, sizeof(pin_buf), "%06lu", (unsigned long)pin_code);
    tft.setTextFont(4);
    tft.setTextColor(C_ACCENT, C_BG);
    tft.setCursor(168, 106);
    tft.print(pin_buf);

    tft.setTextFont(1);
    tft.setTextColor(C_DIM, C_BG);
    tft.setCursor(168, 158);
    tft.print("Or scan QR with");
    tft.setCursor(168, 170);
    tft.print("Glustick app");
}

// ═════════════════════════════════════════════════════════════════════════════
// Heltec V3 — SSD1306 128×64 OLED via U8g2
// ═════════════════════════════════════════════════════════════════════════════
#else

#include <U8g2lib.h>
#include <Wire.h>
#include <qrcode.h>

static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(
    U8G2_R0,
    /* reset= */ OLED_RST,
    /* clock= */ OLED_SCL,
    /* data=  */ OLED_SDA
);

static int   s_rssi    = 0;
static char  s_kid[32] = "";

void display_init() {
    u8g2.begin();
    u8g2.setContrast(180);
}

static void header_bar(const char *title) {
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 7, title);
    u8g2.drawHLine(0, 9, DISPLAY_WIDTH);
}

void display_show_boot() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_8x13B_tf);
    u8g2.drawStr(14, 28, "Glustick");
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(34, 40, "Link");
    u8g2.drawStr(26, 54, FW_VERSION);
    u8g2.sendBuffer();
}

void display_show_provisioning(const char *ble_name) {
    u8g2.clearBuffer();
    header_bar("SETUP");
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 22, "Open Glustick app");
    u8g2.drawStr(0, 32, "and tap 'Add device'");
    u8g2.drawStr(0, 44, "BLE:");
    u8g2.drawStr(20, 44, ble_name);
    u8g2.sendBuffer();
}

void display_show_joining() {
    u8g2.clearBuffer();
    header_bar("CONNECTING");
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 28, "Joining LoRaWAN...");
    u8g2.drawStr(0, 40, "This can take up to");
    u8g2.drawStr(0, 50, "2 minutes.");
    u8g2.sendBuffer();
}

void display_show_idle(const char *kid_name, int rssi, int battery_pct, uint8_t pending) {
    strncpy(s_kid, kid_name, sizeof(s_kid) - 1);
    s_rssi = rssi;

    u8g2.clearBuffer();
    header_bar("GLUSTICK LINK");

    u8g2.setFont(u8g2_font_7x13B_tf);
    u8g2.drawStr(0, 26, kid_name);

    u8g2.setFont(u8g2_font_5x7_tf);
    char rssi_buf[12];
    snprintf(rssi_buf, sizeof(rssi_buf), "%ddBm", rssi);
    u8g2.drawStr(DISPLAY_WIDTH - strlen(rssi_buf) * 5, 26, rssi_buf);

    int bar_w = (battery_pct * 30) / 100;
    u8g2.drawFrame(90, 38, 32, 10);
    u8g2.drawBox(91, 39, bar_w, 8);
    char bat_buf[8];
    snprintf(bat_buf, sizeof(bat_buf), "%d%%", battery_pct);
    u8g2.drawStr(90, 58, bat_buf);

    if (pending > 0) {
        char pend_buf[8];
        snprintf(pend_buf, sizeof(pend_buf), "!%u", pending);
        u8g2.drawStr(0, 40, pend_buf);
    }

    u8g2.sendBuffer();
}

void display_show_message(const char *from, const char *body) {
    u8g2.clearBuffer();
    header_bar("MESSAGE");
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 22, from);
    u8g2.setFont(u8g2_font_5x7_tf);
    char line[22];
    int  body_len = strlen(body);
    int  y = 34;
    for (int i = 0; i < body_len && y <= 60; i += 21) {
        strncpy(line, body + i, 21);
        line[21] = '\0';
        u8g2.drawStr(0, y, line);
        y += 10;
    }
    u8g2.sendBuffer();
}

// No-op on Heltec — no keyboard to compose with
void display_show_compose(const char * /*draft*/) {}

void display_show_sending(uint8_t pending) {
    u8g2.clearBuffer();
    header_bar("SENDING");
    u8g2.setFont(u8g2_font_5x7_tf);
    char buf[32];
    snprintf(buf, sizeof(buf), "%u message(s) queued", pending);
    u8g2.drawStr(0, 28, buf);
    u8g2.drawStr(0, 40, "Transmitting...");
    u8g2.sendBuffer();
}

void display_show_wifi_sync() {
    u8g2.clearBuffer();
    header_bar("WIFI SYNC");
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 28, "WiFi connected.");
    u8g2.drawStr(0, 40, "Syncing messages...");
    u8g2.sendBuffer();
}

void display_show_error(const char *msg) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 10, "ERROR");
    u8g2.drawHLine(0, 12, DISPLAY_WIDTH);
    int len = strlen(msg), y = 24;
    char line[22];
    for (int i = 0; i < len && y <= 60; i += 21) {
        strncpy(line, msg + i, 21);
        line[21] = '\0';
        u8g2.drawStr(0, y, line);
        y += 10;
    }
    u8g2.sendBuffer();
}

void display_set_lora_rssi(int rssi) {
    s_rssi = rssi;
}

void display_show_provisioning_qr(
    const char *dev_eui, const char *app_eui, const char *app_key,
    const char *ble_name, uint32_t pin_code
) {
    char qr_data[72];
    snprintf(qr_data, sizeof(qr_data), "GSF:%s:%s:%s", dev_eui, app_eui, app_key);

    QRCode qrcode;
    uint8_t qrcode_buf[qrcode_getBufferSize(3)];
    qrcode_initText(&qrcode, qrcode_buf, 3, ECC_LOW, qr_data);

    u8g2.clearBuffer();

    u8g2.setDrawColor(1);
    u8g2.drawBox(0, 0, 64, 64);
    u8g2.setDrawColor(0);
    const int x_off = 3, y_off = 3;
    for (uint8_t y = 0; y < qrcode.size; y++) {
        for (uint8_t x = 0; x < qrcode.size; x++) {
            if (qrcode_getModule(&qrcode, x, y)) {
                u8g2.drawBox(x_off + x * 2, y_off + y * 2, 2, 2);
            }
        }
    }

    u8g2.setDrawColor(1);
    u8g2.setFont(u8g2_font_4x6_tf);
    u8g2.drawStr(66, 10, "SCAN QR");
    u8g2.drawStr(66, 22, "BLE:");
    const char *suffix = strrchr(ble_name, '-');
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(66, 32, suffix ? suffix + 1 : ble_name);
    u8g2.setFont(u8g2_font_4x6_tf);
    u8g2.drawStr(66, 46, "PIN:");
    char pin_buf[8];
    snprintf(pin_buf, sizeof(pin_buf), "%06lu", (unsigned long)pin_code);
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(66, 56, pin_buf);

    u8g2.sendBuffer();
}

#endif // BOARD_TDECK
