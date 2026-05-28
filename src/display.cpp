#include "display.h"
#include <U8g2lib.h>
#include <Wire.h>
#include <qrcode.h>

// Heltec V3: SSD1306 128×64 on I²C with hardware reset pin
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(
    U8G2_R0,
    /* reset= */ OLED_RST,
    /* clock= */ OLED_SCL,
    /* data=  */ OLED_SDA
);

static int   s_rssi     = 0;
static char  s_kid[32]  = "";

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

    // Kid name
    u8g2.setFont(u8g2_font_7x13B_tf);
    u8g2.drawStr(0, 26, kid_name);

    // RSSI indicator (right side)
    u8g2.setFont(u8g2_font_5x7_tf);
    char rssi_buf[12];
    snprintf(rssi_buf, sizeof(rssi_buf), "%ddBm", rssi);
    u8g2.drawStr(DISPLAY_WIDTH - strlen(rssi_buf) * 5, 26, rssi_buf);

    // Battery bar
    int bar_w = (battery_pct * 30) / 100;
    u8g2.drawFrame(90, 38, 32, 10);
    u8g2.drawBox(91, 39, bar_w, 8);
    char bat_buf[8];
    snprintf(bat_buf, sizeof(bat_buf), "%d%%", battery_pct);
    u8g2.drawStr(90, 58, bat_buf);

    // Pending messages badge
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
    // Word-wrap body at 21 chars per line
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
    // Word wrap error message
    int len = strlen(msg);
    int y = 24;
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

void display_show_provisioning_qr(const char *dev_eui, const char *app_eui, const char *app_key, const char *ble_name, uint32_t pin_code) {
    // Alphanumeric QR payload: "GSF:{devEUI}:{appEUI}:{appKey}" = 70 chars
    // Version 3 + ECC_LOW supports 77 alphanumeric chars — fits exactly.
    char qr_data[72];
    snprintf(qr_data, sizeof(qr_data), "GSF:%s:%s:%s", dev_eui, app_eui, app_key);

    QRCode qrcode;
    uint8_t qrcode_buf[qrcode_getBufferSize(3)];
    qrcode_initText(&qrcode, qrcode_buf, 3, ECC_LOW, qr_data);

    u8g2.clearBuffer();

    // Left 64px: white background
    u8g2.setDrawColor(1);
    u8g2.drawBox(0, 0, 64, 64);

    // Draw QR modules (black) centered in the white square
    // 29 modules × 2px = 58px; 3px margin on each side within 64px
    u8g2.setDrawColor(0);
    const int x_off = 3, y_off = 3;
    for (uint8_t y = 0; y < qrcode.size; y++) {
        for (uint8_t x = 0; x < qrcode.size; x++) {
            if (qrcode_getModule(&qrcode, x, y)) {
                u8g2.drawBox(x_off + x * 2, y_off + y * 2, 2, 2);
            }
        }
    }

    // Right 62px: BLE name suffix and BLE pairing PIN
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
