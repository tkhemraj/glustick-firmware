// Heltec WiFi LoRa 32 V3 has a 128x64 OLED and one button. It is not a
// handset. This build hears the T-Deck P2P channel and sends each packet
// on once, with the original sender still in the packet.
#ifdef ROLE_REPEATER

#include <Arduino.h>
#include <Preferences.h>
#include "config.h"
#include "display.h"
#include "radio_p2p.h"

#ifndef HELTEC_VEXT
#  define HELTEC_VEXT 36
#endif
#ifndef HELTEC_BTN
#  define HELTEC_BTN 0
#endif
#ifndef HELTEC_ADC_CTRL
#  define HELTEC_ADC_CTRL 37
#endif

static const char *NVS_RPT_NET = "rpt_net";

static int read_battery_pct() {
    pinMode(HELTEC_ADC_CTRL, OUTPUT);
    digitalWrite(HELTEC_ADC_CTRL, HIGH);
    delay(5);
    int raw = analogRead(VBAT_PIN);
    digitalWrite(HELTEC_ADC_CTRL, LOW);
    float v = (raw / 4095.0f) * 3.3f * 2.0f;
    int pct = (int)((v - 3.0f) / 1.2f * 100.0f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

static bool button_held_at_boot() {
    pinMode(HELTEC_BTN, INPUT_PULLUP);
    delay(20);
    if (digitalRead(HELTEC_BTN) != LOW) return false;
    unsigned long t0 = millis();
    while (millis() - t0 < 800) {
        if (digitalRead(HELTEC_BTN) != LOW) return false;
        delay(10);
    }
    return true;
}

static void load_or_forget(char *key_hex) {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    if (button_held_at_boot()) {
        prefs.remove(NVS_RPT_NET);
        prefs.end();
        strcpy(key_hex, "00000000");
        return;
    }
    uint32_t net = prefs.getUInt(NVS_RPT_NET, 0);
    prefs.end();
    snprintf(key_hex, 9, "%08lX", (unsigned long)net);
}

void setup() {
    pinMode(HELTEC_VEXT, OUTPUT);
    digitalWrite(HELTEC_VEXT, LOW);
    delay(20);

    Serial.begin(115200);
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);

    display_init();
    display_show_boot();
    delay(600);

    char key_hex[9];
    load_or_forget(key_hex);
    lora_init("", "", key_hex, nullptr);
    if (!lora_is_joined()) {
        display_show_error("Radio init failed");
        return;
    }
    display_show_repeater(lora_net_id() == 0, lora_net_id(), 0, 0, read_battery_pct());
}

void loop() {
    static uint32_t saved = 0;
    static uint32_t shown_fwd = 0xFFFFFFFFUL;
    static unsigned long last_draw = 0;

    lora_loop();

    uint32_t net = lora_net_id();
    if (net != 0 && net != saved) {
        Preferences prefs;
        prefs.begin(NVS_NAMESPACE, false);
        prefs.putUInt(NVS_RPT_NET, net);
        prefs.end();
        saved = net;
    }

    uint32_t fwd = lora_repeat_count();
    unsigned long now = millis();
    if (fwd != shown_fwd || now - last_draw > 2000) {
        display_show_repeater(net == 0, net, lora_last_rssi(), fwd, read_battery_pct());
        shown_fwd = fwd;
        last_draw = now;
    }
}

#endif
