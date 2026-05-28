#include "wifi_sync.h"
#include "config.h"
#include "display.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

static char         s_server_url[128]   = {};
static char         s_parent_tok[256]   = {};
static char         s_server_cert[2048] = {};  // PEM cert for TLS pinning, or empty
static InboundMsgCb s_inbound_cb        = nullptr;
static unsigned long s_last_sync        = 0;

bool wifi_sync_init(
    const char *ssid,
    const char *password,
    const char *server_url,
    const char *parent_token,
    const char *server_cert,
    InboundMsgCb on_inbound
) {
    strncpy(s_server_url,  server_url,   sizeof(s_server_url)  - 1);
    strncpy(s_parent_tok,  parent_token, sizeof(s_parent_tok)  - 1);
    strncpy(s_server_cert, server_cert,  sizeof(s_server_cert) - 1);
    s_inbound_cb = on_inbound;

    if (strlen(ssid) == 0) return false;

    WiFi.begin(ssid, password);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < 15000) {
        delay(500);
    }
    return WiFi.status() == WL_CONNECTED;
}

bool wifi_sync_connected() {
    return WiFi.status() == WL_CONNECTED;
}

static void apply_tls(WiFiClientSecure &client) {
    if (strlen(s_server_cert) > 0) {
        client.setCACert(s_server_cert);
    } else {
        // No cert provisioned — connections are unauthenticated.
        // Provision the server PEM cert via the Glustick app to enable pinning.
        client.setInsecure();
    }
}

static bool http_post_message(const char *body) {
    if (!wifi_sync_connected()) return false;

    WiFiClientSecure client;
    apply_tls(client);
    HTTPClient http;

    char url[192];
    snprintf(url, sizeof(url), "%s/api/v1/lora/uplink", s_server_url);
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + s_parent_tok);

    JsonDocument doc;
    doc["body"] = body;
    char payload[MSG_BODY_MAX + 64];
    serializeJson(doc, payload, sizeof(payload));

    int code = http.POST(payload);
    http.end();
    return code >= 200 && code < 300;
}

static void http_poll_inbound() {
    if (!wifi_sync_connected() || !s_inbound_cb) return;

    WiFiClientSecure client;
    apply_tls(client);
    HTTPClient http;

    char url[192];
    snprintf(url, sizeof(url), "%s/api/v1/lora/downlink", s_server_url);
    http.begin(client, url);
    http.addHeader("Authorization", String("Bearer ") + s_parent_tok);

    int code = http.GET();
    if (code == 200) {
        String resp = http.getString();
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, resp);
        if (!err && doc.is<JsonArray>()) {
            for (JsonVariant item : doc.as<JsonArray>()) {
                const char *body = item["body"] | "";
                if (strlen(body) > 0) s_inbound_cb(body);
            }
        }
    }
    http.end();
}

void wifi_sync_flush() {
    QueuedMessage msg;
    while (queue_pop_outbound(&msg)) {
        if (http_post_message(msg.body)) {
            queue_mark_acked(msg.id);
        } else {
            break;
        }
    }
    queue_flush_acked();
    http_poll_inbound();
}

void wifi_sync_loop() {
    if (!wifi_sync_connected()) return;
    if (millis() - s_last_sync >= WIFI_SYNC_INTERVAL_MS) {
        display_show_wifi_sync();
        wifi_sync_flush();
        s_last_sync = millis();
    }
}
