#pragma once
#ifdef BOARD_TDECK

#include <Arduino.h>

enum TrackDir { TDIR_NONE, TDIR_UP, TDIR_DOWN, TDIR_LEFT, TDIR_RIGHT };

void     trackball_init();
TrackDir trackball_poll();  // returns direction pressed this call, NONE if idle

#endif // BOARD_TDECK
