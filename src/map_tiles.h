#pragma once
#ifdef GLUSTIC_FUN

#include <Arduino.h>
#include "gps.h"

// Initialise SD card and tile renderer.  Returns false if SD is not found.
bool map_tiles_init();

// Render the map centred on `own` with a dot for each device.
// peer.valid=false → peer dot not drawn.
// Fills the full 320×240 display; call from the map view only.
void map_tiles_draw(GpsCoord own, GpsCoord peer);

// Change zoom at runtime (1–19; recommend 15–17 for neighbourhood use).
void map_tiles_set_zoom(uint8_t zoom);

#endif // GLUSTIC_FUN
