#include "provisioning.h"
#include "config.h"
#include "display.h"
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLESecurity.h>
#include <esp_gap_ble_api.h>
#include <esp_system.h>
#include <freertos/semphr.h>

// ── BLE service / characteristic UUIDs ───────────────────────────────────────
#define PROV_SERVICE_UUID    "12345678-1234-1234-1234-123456789abc"
#define CHAR_DEV_EUI_UUID    "12345678-1234-1234-1234-000000000001"
#define CHAR_APP_EUI_UUID    "12345678-1234-1234-1234-000000000002"
#define CHAR_APP_KEY_UUID    "12345678-1234-1234-1234-000000000003"
#define CHAR_SERVER_URL_UUID "12345678-1234-1234-1234-000000000004"
#define CHAR_PARENT_TOK_UUID "12345678-1234-1234-1234-000000000005"
#define CHAR_WIFI_SSID_UUID  "12345678-1234-1234-1234-000000000006"
#define CHAR_WIFI_PASS_UUID  "12345678-1234-1234-1234-000000000007"
#define CHAR_KID_NAME_UUID   "12345678-1234-1234-1234-000000000008"
#define CHAR_COMMIT_UUID     "12345678-1234-1234-1234-000000000009"
#define CHAR_SERVER_CERT_UUID "12345678-1234-1234-1234-00000000000a"

static volatile bool     s_provisioned = false;
static Preferences       s_prefs;
static SemaphoreHandle_t s_mux         = nullptr;

// Staged values — written to NVS atomically when the app commits
static char s_dev_eui[17]    = {};
static char s_app_eui[17]    = {};
static char s_app_key[33]    = {};
static char s_server_url[128] = {};
static char s_parent_tok[256] = {};
static char s_wifi_ssid[64]  = {};
static char s_wifi_pass[64]  = {};
static char s_kid_name[32]   = {};
static char s_server_cert[2048] = {};  // PEM cert for TLS pinning (optional)

// ── BLE characteristic write callback ────────────────────────────────────────
class ProvCharCallback : public BLECharacteristicCallbacks {
    char   *m_buf;
    size_t  m_len;
public:
    ProvCharCallback(char *buf, size_t len) : m_buf(buf), m_len(len) {}
    void onWrite(BLECharacteristic *c) override {
        std::string v = c->getValue();
        size_t copy_len = v.length() < m_len - 1 ? v.length() : m_len - 1;
        xSemaphoreTake(s_mux, portMAX_DELAY);
        memcpy(m_buf, v.c_str(), copy_len);
        m_buf[copy_len] = '\0';
        xSemaphoreGive(s_mux);
    }
};

// ── Commit callback: validates and persists all staged values atomically ─────
class CommitCallback : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override {
        xSemaphoreTake(s_mux, portMAX_DELAY);

        bool valid = (strlen(s_dev_eui) == 16 && strlen(s_app_eui) == 16
                   && strlen(s_app_key) == 32  && strlen(s_server_url) > 0
                   && strlen(s_parent_tok) > 0);
        if (!valid) {
            xSemaphoreGive(s_mux);
            return;
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
        s_prefs.putString(NVS_KEY_SERVER_CERT, s_server_cert);
        s_prefs.putBool(NVS_KEY_PROVISIONED, true);
        s_prefs.end();
        s_provisioned = true;

        xSemaphoreGive(s_mux);
    }
};

// ── Derive factory defaults on first boot ────────────────────────────────────
void provisioning_derive_defaults() {
    Preferences p;
    p.begin(NVS_NAMESPACE, true);
    bool already_set = p.getBool("factory_set", false);
    p.end();
    if (already_set) return;

    // EUI-64 from ESP32 chip MAC.
    // getEfuseMac() returns the 48-bit MAC as a little-endian uint64:
    // bit[47:40] = mac[0] (OUI first byte), bit[7:0] = mac[5].
    uint64_t chip_id = ESP.getEfuseMac();
    uint8_t mac[6];
    mac[0] = (chip_id >> 40) & 0xFF;  // OUI byte 0 — U/L bit lives here
    mac[1] = (chip_id >> 32) & 0xFF;
    mac[2] = (chip_id >> 24) & 0xFF;
    mac[3] = (chip_id >> 16) & 0xFF;
    mac[4] = (chip_id >>  8) & 0xFF;
    mac[5] = (chip_id      ) & 0xFF;

    char dev_eui[17];
    snprintf(dev_eui, sizeof(dev_eui), "%02X%02X%02XFFFE%02X%02X%02X",
        mac[0] ^ 0x02, mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Cryptographically random AppKey — never derivable from public device info.
    // The QR code carries this key so the app can pre-fill it; BLE allows override.
    uint8_t key[16];
    esp_fill_random(key, sizeof(key));
    char app_key[33];
    for (int i = 0; i < 16; i++) snprintf(app_key + i * 2, 3, "%02X", key[i]);

    p.begin(NVS_NAMESPACE, false);
    p.putString(NVS_KEY_DEV_EUI, dev_eui);
    p.putString(NVS_KEY_APP_EUI, "0000000000000000");
    p.putString(NVS_KEY_APP_KEY, app_key);
    p.putBool("factory_set", true);
    p.end();
}

// ── BLE provisioning wizard ───────────────────────────────────────────────────
bool provisioning_run() {
    if (!s_mux) s_mux = xSemaphoreCreateMutex();

    uint64_t chip_id = ESP.getEfuseMac();
    char ble_name[24];
    snprintf(ble_name, sizeof(ble_name), "GsfLink-%04X", (uint16_t)(chip_id & 0xFFFF));

    // Load factory defaults for QR display
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

    // Random 6-digit PIN displayed on OLED — required for BLE pairing
    uint32_t pin = esp_random() % 900000 + 100000;
    display_show_provisioning_qr(qr_dev_eui, qr_app_eui, qr_app_key, ble_name, pin);

    BLEDevice::init(ble_name);

    // Require authenticated encrypted connection before any characteristic access.
    // ESP_IO_CAP_OUT: we display the passkey; the phone user enters it.
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_STATIC_PASSKEY, &pin, sizeof(uint32_t));
    BLESecurity *sec = new BLESecurity();
    sec->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
    sec->setCapability(ESP_IO_CAP_OUT);
    sec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

    BLEServer  *server = BLEDevice::createServer();
    BLEService *svc    = server->createService(PROV_SERVICE_UUID);

    auto make_char = [&](const char *uuid, char *buf, size_t len) {
        auto *c = svc->createCharacteristic(uuid,
            BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_READ);
        c->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED);
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
    make_char(CHAR_SERVER_CERT_UUID, s_server_cert, sizeof(s_server_cert));

    auto *commit_char = svc->createCharacteristic(CHAR_COMMIT_UUID,
        BLECharacteristic::PROPERTY_WRITE);
    commit_char->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);
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
    char *kid_name, char *tls_fp
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
        p.getString(NVS_KEY_SERVER_CERT, tls_fp,     2048);
    }
    p.end();
    return ok;
}
