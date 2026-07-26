#ifdef BOARD_TDECK

#include "trackball.h"
#include <Arduino.h>

// T-Deck TBOX navigation ball — 4 active-LOW GPIO pins.
// Pin mapping from LILYGO utilities.h (BOARD_TBOX_G0x):
//   G01=GPIO2, G02=GPIO3, G03=GPIO15, G04=GPIO1
// Physical direction depends on device orientation; swap defines if inverted.
#ifndef TBOX_UP
#  define TBOX_UP    3    // G02 = GPIO 3
#endif
#ifndef TBOX_DOWN
#  define TBOX_DOWN  15   // G03 = GPIO 15
#endif
#ifndef TBOX_LEFT
#  define TBOX_LEFT  2    // G01 = GPIO 2
#endif
#ifndef TBOX_RIGHT
#  define TBOX_RIGHT 1    // G04 = GPIO 1
#endif

static constexpr uint32_t DEBOUNCE_MS = 80;
static uint8_t  s_last     = 0;
static uint32_t s_last_ms  = 0;

static uint8_t read_raw() {
    uint8_t v = 0;
    if (digitalRead(TBOX_UP)    == LOW) v |= 0x01;
    if (digitalRead(TBOX_DOWN)  == LOW) v |= 0x02;
    if (digitalRead(TBOX_LEFT)  == LOW) v |= 0x04;
    if (digitalRead(TBOX_RIGHT) == LOW) v |= 0x08;
    return v;
}

void trackball_init() {
    pinMode(TBOX_UP,    INPUT_PULLUP);
    pinMode(TBOX_DOWN,  INPUT_PULLUP);
    pinMode(TBOX_LEFT,  INPUT_PULLUP);
    pinMode(TBOX_RIGHT, INPUT_PULLUP);
}

TrackDir trackball_poll() {
    uint8_t raw = read_raw();
    uint32_t now = millis();

    // Only fire when a new press is detected (edge, with debounce)
    if (raw == s_last) return TDIR_NONE;
    if (now - s_last_ms < DEBOUNCE_MS) return TDIR_NONE;

    s_last    = raw;
    s_last_ms = now;

    if (raw == 0) return TDIR_NONE;  // it was a release

    if (raw & 0x01) return TDIR_UP;
    if (raw & 0x02) return TDIR_DOWN;
    if (raw & 0x04) return TDIR_LEFT;
    if (raw & 0x08) return TDIR_RIGHT;
    return TDIR_NONE;
}

#endif // BOARD_TDECK
