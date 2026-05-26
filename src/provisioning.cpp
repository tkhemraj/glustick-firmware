#include "provisioning.h"
#include "config.h"
#include "display.h"
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ── BLE service / characteristic UUIDs ───────────────────────────────────────
// Custom 128-bit UUIDs for the Glustick provisioning service.
#define PROV_SERVICE_UUID   "12345678-1234-1234-1234-123456789abc"
#define CHAR_DEV_EUI_UUID   "12345678-1234-1234-1234-000000000001"
#define CHAR_APP_EUI_UUID   "12345678-1234-1234-1234-000000000002"
#define CHAR_APP_KEY_UUID   "12345678-1234-1234-1234-000000000003"
#define CHAR_SERVER_URL_UUID "12345678-1234-1234-1234-000000000004"
#define CHAR_PARENT_TOK_UUID "12345678-1234-1234-1234-000000000005"
#define CHAR_WIFI_SSID_UUID  "12345678-1234-1234-1234-000000000006"
#define CHAR_WIFI_PASS_UUID  "12345678-1234-1234-1234-000000000007"
#define CHAR_KID_NAME_UUID   "12345678-1234-1234-1234-000000000008"
#define CHAR_COMMIT_UUID     "12345678-1234-1234-1234-000000000009"

static volatile bool s_provisioned = false;
static Preferences   s_prefs;

// Staged values — written to NVS when the app writes to CHAR_COMMIT_UUID
static char s_dev_eui[17]   = {};
static char s_app_eui[17]   = {};
static char s_app_key[33]   = {};
static char s_server_url[128] = {};
static char s_parent_tok[256] = {};
static char s_wifi_ssid[64] = {};
static char s_wifi_pass[64] = {};
static char s_kid_name[32]  = {};

class ProvCharCallback : public BLECharacteristicCallbacks {
    char *m_buf;
    size_t m_len;
public:
    ProvCharCallback(char *buf, size_t len) : m_buf(buf), m_len(len) {}
    void onWrite(BLECharacteristic *c) override {
        std::string v = c->getValue();
        size_t copy_len = v.length() < m_len - 1 ? v.length() : m_len - 1;
        memcpy(m_buf, v.c_str(), copy_len);
        m_buf[copy_len] = '\0';
    }
};

class CommitCallback : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override {
        // App writes "1" to commit — validate and persist
        if (strlen(s_dev_eui) != 16 || strlen(s_app_eui) != 16 || strlen(s_app_key) != 32) {
            return; // silently ignore incomplete provisioning
        }
        s_prefs.begin(NVS_NAMESPACE, false);
        s_prefs.putString(NVS_KEY_DEV_EUI,    s_dev_eui);
        s_prefs.putString(NVS_KEY_APP_EUI,    s_app_eui);
        s_prefs.putString(NVS_KEY_APP_KEY,    s_app_key);
        s_prefs.putString(NVS_KEY_SERVER_URL, s_server_url);
        s_prefs.putString(NVS_KEY_PARENT_TOK, s_parent_tok);
        s_prefs.putString(NVS_KEY_WIFI_SSID,  s_wifi_ssid);
        s_prefs.putString(NVS_KEY_WIFI_PASS,  s_wifi_pass);
        s_prefs.putString(NVS_KEY_KID_NAME,   s_kid_name);
        s_prefs.putBool(NVS_KEY_PROVISIONED, true);
        s_prefs.end();
        s_provisioned = true;
    }
};

void provisioning_derive_defaults() {
    Preferences p;
    p.begin(NVS_NAMESPACE, true);
    bool already_set = p.getBool("factory_set", false);
    p.end();
    if (already_set) return;

    // EUI-64 from ESP32 chip MAC: insert FF:FE at bytes 3-4, XOR bit 1 of byte 0
    uint64_t chip_id = ESP.getEfuseMac();
    uint8_t mac[6];
    mac[0] = (chip_id      ) & 0xFF;
    mac[1] = (chip_id >>  8) & 0xFF;
    mac[2] = (chip_id >> 16) & 0xFF;
    mac[3] = (chip_id >> 24) & 0xFF;
    mac[4] = (chip_id >> 32) & 0xFF;
    mac[5] = (chip_id >> 40) & 0xFF;

    char dev_eui[17];
    snprintf(dev_eui, sizeof(dev_eui), "%02X%02X%02XFFFE%02X%02X%02X",
        mac[0] ^ 0x02, mac[1], mac[2], mac[3], mac[4], mac[5]);

    // AppKey: deterministic from chip ID (dev/unboxed default; production uses factory-burned keys)
    uint8_t key[16];
    for (int i = 0; i < 8; i++) {
        uint8_t b = (chip_id >> (i * 8)) & 0xFF;
        key[i]     = b;
        key[i + 8] = b ^ 0x5A;
    }
    char app_key[33];
    for (int i = 0; i < 16; i++) snprintf(app_key + i * 2, 3, "%02X", key[i]);

    p.begin(NVS_NAMESPACE, false);
    p.putString(NVS_KEY_DEV_EUI, dev_eui);
    p.putString(NVS_KEY_APP_EUI, "0000000000000000");
    p.putString(NVS_KEY_APP_KEY, app_key);
    p.putBool("factory_set", true);
    p.end();
}

bool provisioning_run() {
    // Build BLE device name from last 4 hex chars of chip ID
    uint64_t chip_id = ESP.getEfuseMac();
    char ble_name[24];
    snprintf(ble_name, sizeof(ble_name), "GsfLink-%04X", (uint16_t)(chip_id & 0xFFFF));

    // Load factory defaults for QR display (set by provisioning_derive_defaults)
    char qr_dev_eui[17] = {};
    char qr_app_eui[17] = {};
    char qr_app_key[33] = {};
    {
        Preferences p;
        p.begin(NVS_NAMESPACE, true);
        p.getString(NVS_KEY_DEV_EUI, qr_dev_eui, sizeof(qr_dev_eui));
        p.getString(NVS_KEY_APP_EUI, qr_app_eui, sizeof(qr_app_eui));
        p.getString(NVS_KEY_APP_KEY, qr_app_key, sizeof(qr_app_key));
        p.end();
    }

    display_show_provisioning_qr(qr_dev_eui, qr_app_eui, qr_app_key, ble_name);

    BLEDevice::init(ble_name);
    BLEServer *server = BLEDevice::createServer();
    BLEService *svc   = server->createService(PROV_SERVICE_UUID);

    auto make_char = [&](const char *uuid, char *buf, size_t len) {
        auto *c = svc->createCharacteristic(uuid, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_READ);
        c->setCallbacks(new ProvCharCallback(buf, len));
        return c;
    };

    make_char(CHAR_DEV_EUI_UUID,    s_dev_eui,    sizeof(s_dev_eui));
    make_char(CHAR_APP_EUI_UUID,    s_app_eui,    sizeof(s_app_eui));
    make_char(CHAR_APP_KEY_UUID,    s_app_key,    sizeof(s_app_key));
    make_char(CHAR_SERVER_URL_UUID, s_server_url, sizeof(s_server_url));
    make_char(CHAR_PARENT_TOK_UUID, s_parent_tok, sizeof(s_parent_tok));
    make_char(CHAR_WIFI_SSID_UUID,  s_wifi_ssid,  sizeof(s_wifi_ssid));
    make_char(CHAR_WIFI_PASS_UUID,  s_wifi_pass,  sizeof(s_wifi_pass));
    make_char(CHAR_KID_NAME_UUID,   s_kid_name,   sizeof(s_kid_name));

    auto *commit_char = svc->createCharacteristic(CHAR_COMMIT_UUID, BLECharacteristic::PROPERTY_WRITE);
    commit_char->setCallbacks(new CommitCallback());

    svc->start();

    BLEAdvertising *adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(PROV_SERVICE_UUID);
    adv->setScanResponse(true);
    BLEDevice::startAdvertising();

    unsigned long start = millis();
    while (!s_provisioned && (millis() - start) < BLE_TIMEOUT_MS) {
        delay(100);
    }

    BLEDevice::stopAdvertising();
    BLEDevice::deinit(true);

    return s_provisioned;
}

bool provisioning_is_complete() {
    Preferences p;
    p.begin(NVS_NAMESPACE, true);
    bool done = p.getBool(NVS_KEY_PROVISIONED, false);
    p.end();
    return done;
}

bool provisioning_load(
    char *dev_eui, char *app_eui, char *app_key,
    char *server_url, char *parent_tok,
    char *wifi_ssid, char *wifi_pass,
    char *kid_name
) {
    Preferences p;
    p.begin(NVS_NAMESPACE, true);
    bool ok = p.getBool(NVS_KEY_PROVISIONED, false);
    if (ok) {
        p.getString(NVS_KEY_DEV_EUI,    dev_eui,    17);
        p.getString(NVS_KEY_APP_EUI,    app_eui,    17);
        p.getString(NVS_KEY_APP_KEY,    app_key,    33);
        p.getString(NVS_KEY_SERVER_URL, server_url, 128);
        p.getString(NVS_KEY_PARENT_TOK, parent_tok, 256);
        p.getString(NVS_KEY_WIFI_SSID,  wifi_ssid,  64);
        p.getString(NVS_KEY_WIFI_PASS,  wifi_pass,  64);
        p.getString(NVS_KEY_KID_NAME,   kid_name,   32);
    }
    p.end();
    return ok;
}
