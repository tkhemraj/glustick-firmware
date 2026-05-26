#pragma once
#include <Arduino.h>

// Derives DevEUI (EUI-64 from chip MAC), AppEUI (all zeros), and AppKey
// (deterministic from chip ID) and writes them to NVS as factory defaults.
// No-op if already set. Call once before provisioning_run() so the QR code
// on the display is populated even before BLE setup completes.
void provisioning_derive_defaults();

// BLE provisioning wizard. Call on first boot (NVS_KEY_PROVISIONED not set).
// Advertises a BLE GATT service; the Glustick mobile app connects and writes
// the device credentials + server config. Blocks until done or timeout.
//
// Returns true if successfully provisioned, false if timed out.
bool provisioning_run();

// Returns true if provisioning data is already in NVS.
bool provisioning_is_complete();

// Read provisioned values from NVS into out-params (all must be non-null).
// Buffers should be at least 64 bytes for URLs/tokens and 33 for keys.
bool provisioning_load(
    char *dev_eui,    // 17 bytes (16 hex + null)
    char *app_eui,    // 17 bytes
    char *app_key,    // 33 bytes
    char *server_url, // 128 bytes
    char *parent_tok, // 256 bytes
    char *wifi_ssid,  // 64 bytes
    char *wifi_pass,  // 64 bytes
    char *kid_name    // 32 bytes
);
