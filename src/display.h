#pragma once
#include <Arduino.h>
#include "config.h"

void display_init();
void display_show_boot();
void display_show_provisioning(const char *ble_name);
void display_show_joining();
void display_show_idle(const char *kid_name, int rssi, int battery_pct, uint8_t pending);
void display_show_message(const char *from, const char *body);
void display_show_sending(uint8_t pending);
void display_show_wifi_sync();
void display_show_error(const char *msg);
void display_set_lora_rssi(int rssi);
void display_show_provisioning_qr(const char *dev_eui, const char *app_eui, const char *app_key, const char *ble_name);
