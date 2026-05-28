#pragma once
#include <Arduino.h>
#include "message_queue.h"

typedef void (*InboundMsgCb)(const char *body);

// Connect to WiFi and start periodic sync with the Glustick server.
// server_cert: PEM cert for TLS pinning via setCACert(); empty = setInsecure().
bool wifi_sync_init(
    const char *ssid,
    const char *password,
    const char *server_url,
    const char *parent_token,
    const char *server_cert,
    InboundMsgCb on_inbound
);

// Call from loop() when WiFi is connected.
void wifi_sync_loop();

// Returns true if currently connected to WiFi.
bool wifi_sync_connected();

// Flush any outbound messages in the queue over HTTPS immediately.
void wifi_sync_flush();
