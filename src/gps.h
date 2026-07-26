#pragma once
#ifdef GLUSTIC_FUN

#include <Arduino.h>

struct GpsCoord {
    double  lat;
    double  lon;
    bool    valid;
    uint8_t satellites;
};

// Call once in setup() — disables Serial to free GPIO 43/44 for GPS UART.
void     gps_init();

// Call every loop() — feeds NMEA bytes from the GPS module into TinyGPS++.
void     gps_loop();

// Returns the latest fix.  valid=false until the first fix is acquired.
GpsCoord gps_get();

#endif // GLUSTIC_FUN
