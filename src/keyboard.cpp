#ifdef BOARD_TDECK

#include "keyboard.h"
#include <Wire.h>

void keyboard_init() {
    Wire.begin(KB_SDA, KB_SCL);
    Wire.setClock(100000);
    pinMode(KB_INT, INPUT_PULLUP);
    // LILYGO keyboard MCU (ESP32-C3) needs 500 ms after power-on to be ready
    delay(500);
    // Set keyboard backlight to full brightness
    Wire.beginTransmission(KB_ADDR);
    Wire.write(0x01);   // BRIGHTNESS_CMD
    Wire.write(255);
    Wire.endTransmission();
}

char keyboard_read() {
    // Poll unconditionally — the INT check was masking keys when the
    // pin was floating or the MCU hadn't asserted it yet.
    Wire.requestFrom((uint8_t)KB_ADDR, (uint8_t)1);
    if (!Wire.available()) return 0;
    uint8_t raw = Wire.read();
    if (raw == 0) return 0;
    return (char)raw;
}

#endif // BOARD_TDECK
