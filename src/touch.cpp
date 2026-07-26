#ifdef BOARD_TDECK

#include "touch.h"
#include <Wire.h>

// GT911 capacitive touch controller
// I2C address is set by INT pin state during reset:
//   INT = LOW  → 0x5D (LILYGO T-Deck default)
//   INT = HIGH → 0x14
// INT pin: GPIO 16 (BOARD_TOUCH_INT)

static constexpr uint8_t  GT_ADDR_L   = 0x5D;
static constexpr uint8_t  GT_ADDR_H   = 0x14;
static constexpr uint8_t  GT_INT_PIN  = 16;

// Key GT911 registers (16-bit address, big-endian)
static constexpr uint16_t GT_PRODID   = 0x8140;  // "911\0" product ID
static constexpr uint16_t GT_STATUS   = 0x814E;  // buffer-ready + finger count
static constexpr uint16_t GT_TOUCH1   = 0x8150;  // first touch point data (8 bytes)

static uint8_t s_addr = 0;  // 0 = not found

static bool gt_write(uint16_t reg, uint8_t val) {
    Wire.beginTransmission(s_addr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool gt_read(uint16_t reg, uint8_t *buf, uint8_t len) {
    Wire.beginTransmission(s_addr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    if (Wire.endTransmission(false) != 0) return false;
    Wire.requestFrom(s_addr, len);
    for (int i = 0; Wire.available() && i < len; i++) buf[i] = Wire.read();
    return true;
}

static bool probe(uint8_t addr) {
    s_addr = addr;
    uint8_t id[4] = {};
    if (!gt_read(GT_PRODID, id, 4)) return false;
    // GT911 product ID reads as "911" or "9110"
    return (id[0] == '9' && id[1] == '1' && id[2] == '1');
}

bool touch_init() {
    pinMode(GT_INT_PIN, INPUT);
    delay(20);
    if (probe(GT_ADDR_L)) return true;
    if (probe(GT_ADDR_H)) return true;
    s_addr = 0;
    return false;
}

bool touch_read(int16_t *x, int16_t *y) {
    if (!s_addr) return false;

    uint8_t status = 0;
    if (!gt_read(GT_STATUS, &status, 1)) return false;

    // Bit 7 = buffer ready, bits 3:0 = finger count
    if (!(status & 0x80)) return false;
    uint8_t n = status & 0x0F;

    // Always clear the buffer-ready flag
    gt_write(GT_STATUS, 0);

    if (n == 0) return false;

    // Touch point format: [track_id][xL][xH][yL][yH][size][--][--]
    uint8_t d[6];
    if (!gt_read(GT_TOUCH1, d, 6)) return false;

    int16_t raw_x = ((int16_t)(d[2] & 0x0F) << 8) | d[1];
    int16_t raw_y = ((int16_t)(d[4] & 0x0F) << 8) | d[3];

    // T-Deck transform: setSwapXY(true) + setMirrorXY(false, true)
    *x = raw_y;
    *y = 239 - raw_x;

    return (*x >= 0 && *x < 320 && *y >= 0 && *y < 240);
}

#endif // BOARD_TDECK
