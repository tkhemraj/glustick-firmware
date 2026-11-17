#pragma once
// ── Remembered Wi-Fi networks ────────────────────────────────────────────────
// Persists every network the device has successfully joined (up to 8) in NVS,
// ranked by a monotonic counter so "most recently used" survives reboots. The
// device never forgets a network unless it is explicitly deleted. Only the
// WiFi-capable build needs this.
#if defined(TRANSPORT_MESHTASTIC)

#include <Arduino.h>

#define WIFI_STORE_MAX   8
#define WIFI_SSID_LEN    33   // 32 + NUL
#define WIFI_PASS_LEN    65   // 64 + NUL

// Load the saved table from NVS. Call once in setup() before any other call.
void wifi_store_begin();

// Number of remembered networks.
int  wifi_store_count();

// Fill idx_out[0..n) with table indices ordered most-recently-used first.
// Returns n (<= max, <= count). Use with wifi_store_ssid()/_pass().
int  wifi_store_by_recency(int *idx_out, int max);

const char *wifi_store_ssid(int idx);
const char *wifi_store_pass(int idx);

// Remember a network after a successful join: insert if new (evicting the
// least-recently-used when full), refresh the password if changed, and bump it
// to most-recently-used. Empty ssid is ignored. Persists immediately.
void wifi_store_remember(const char *ssid, const char *pass);

// Bump a known ssid to most-recently-used without changing its password.
// No-op if the ssid isn't stored. Persists immediately.
void wifi_store_touch(const char *ssid);

// Delete workflow: remove a network so it IS forgotten. Returns true if removed.
bool wifi_store_delete(const char *ssid);
bool wifi_store_delete_idx(int idx);

#endif // TRANSPORT_MESHTASTIC
