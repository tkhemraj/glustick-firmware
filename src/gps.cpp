#if defined(GLUSTIC_FUN) || defined(TRANSPORT_MESHTASTIC)

#include "gps.h"
#include <TinyGPS++.h>
#include <HardwareSerial.h>

static TinyGPSPlus   s_gps;
static HardwareSerial s_serial(1);  // UART1 — remapped to GPS_RX_PIN / GPS_TX_PIN

void gps_init() {
#ifndef TRANSPORT_MESHTASTIC
    // GLUSTIC_FUN: Serial = UART0 on GPIO 43/44 — end it to free the pins.
    // remotest uses USB CDC (Serial = HWCDC), so UART0 doesn't own those pins.
    Serial.end();
#endif
    s_serial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
}

void gps_loop() {
    while (s_serial.available()) {
        s_gps.encode(s_serial.read());
    }
}

GpsCoord gps_get() {
    bool valid = s_gps.location.isValid() && s_gps.location.age() < 5000;
    return {
        s_gps.location.lat(),
        s_gps.location.lng(),
        valid,
        (uint8_t)(s_gps.satellites.isValid() ? s_gps.satellites.value() : 0)
    };
}

#endif // GLUSTIC_FUN || TRANSPORT_MESHTASTIC
