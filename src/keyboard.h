#pragma once
#ifdef BOARD_TDECK

#include <Arduino.h>

// Initialise the T-Deck I²C keyboard (Wire1 on KB_SDA/KB_SCL).
void keyboard_init();

// Return the next key character, or 0 if nothing is pending.
// Special values: '\b' (0x08) = backspace, '\r' (0x0D) = enter/send.
char keyboard_read();

#endif // BOARD_TDECK
