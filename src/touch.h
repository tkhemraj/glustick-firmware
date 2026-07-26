#pragma once
#ifdef BOARD_TDECK

#include <Arduino.h>

// Initialise GT911 touch controller.  Returns true if detected.
bool touch_init();

// Poll for a touch.  Returns true and fills x/y if a finger is down.
// Coordinates are in display space: x 0-319, y 0-239.
bool touch_read(int16_t *x, int16_t *y);

#endif // BOARD_TDECK
