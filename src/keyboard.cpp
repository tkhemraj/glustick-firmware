#ifdef BOARD_TDECK

#include "keyboard.h"
#include <Wire.h>

void keyboard_init() {
    // T-Deck keyboard sits on a dedicated I²C bus (Wire1)
    Wire1.begin(KB_SDA, KB_SCL);
    Wire1.setClock(100000);

    // Pull INT pin to detect key-available signal
    pinMode(KB_INT, INPUT_PULLUP);
}

char keyboard_read() {
    // INT is active-LOW when a key is waiting
    if (digitalRead(KB_INT) == HIGH) return 0;

    Wire1.requestFrom((uint8_t)KB_ADDR, (uint8_t)1);
    if (!Wire1.available()) return 0;

    uint8_t raw = Wire1.read();
    if (raw == 0) return 0;

    // Map the T-Deck keyboard's special codes to standard ASCII:
    //   0x08 = backspace (already standard)
    //   0x0D = enter     (already standard)
    //   0x20+ = printable ASCII passthrough
    return (char)raw;
}

#endif // BOARD_TDECK
