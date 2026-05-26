# Glustick Link — Firmware

Firmware for the **Glustick Link** off-grid messaging device. Built with [PlatformIO](https://platformio.org/) targeting the **Heltec WiFi LoRa 32 V3** (ESP32-S3 + SX1262 + SSD1306 OLED).

Glustick Link lets families communicate when the internet is out: messages travel over LoRaWAN to a gateway, through ChirpStack, and into the Glustick Family server. WiFi is used for faster sync when available.

---

## Hardware

| Component | Details |
|---|---|
| MCU | ESP32 (Heltec WiFi LoRa 32 V3 — `heltec_wifi_lora_32_V3`) |
| Radio | SX1262 (LoRa 868/915 MHz) |
| Display | SSD1306 128×64 OLED (I²C) |
| Connectivity | WiFi 2.4 GHz + BLE 5.0 + LoRaWAN |
| Power | USB-C + LiPo header |

---

## Building

Install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html) or the [VS Code extension](https://platformio.org/install/ide?install=vscode).

```bash
# US915 (North America) — default
pio run -e heltec_v3

# EU868 (Europe)
pio run -e heltec_v3_eu868

# Flash
pio run -e heltec_v3 --target upload

# Serial monitor
pio device monitor
```

---

## First boot — provisioning

On first boot the device enters **provisioning mode** and advertises a BLE service named `GsfLink-XXXX` (last 4 hex chars of chip ID).

1. Open the **Glustick Family** mobile app
2. Go to Settings → Glustick Link → Add device
3. The app connects via BLE and writes:
   - DevEUI, AppEUI, AppKey (from the server registration)
   - Your Glustick server URL + parent token
   - WiFi credentials (optional but recommended)
   - Kid's name (shown on OLED)
4. App writes `1` to the commit characteristic → device saves to NVS and reboots into normal operation

Provisioning times out after **5 minutes**. Power cycle to retry.

---

## Frame format

Matches `apps/api/internal/lora/chunker.go` in the server repo exactly.

```
Byte 0:   version    = 1
Byte 1:   type       = 0 (msg) | 1 (ack) | 2 (ping)
Byte 2-3: msgID      uint16 little-endian
Byte 4:   chunkIdx
Byte 5:   chunkTotal
Byte 6+:  payload    max 44 bytes
```

Messages ≤ 44 bytes fit in a single frame. Longer messages are chunked; the server reassembles using Redis with a 120 s TTL.

---

## QR code

The sticker on each device encodes:

```
gsf:{devEUI}:{appEUI}:{appKey}
```

The Glustick Family mobile app can scan this to pre-fill the registration form.

---

## State machine

```
PROVISIONING  ──→  JOINING  ──→  LORA_CONNECTED
                               ↕
                          WIFI_CONNECTED  ←──→  IDLE
```

- **PROVISIONING** — BLE advertising, waiting for mobile app
- **JOINING** — OTAA join in progress (up to 2 min)
- **LORA_CONNECTED** — joined, no WiFi; messages go over LoRa
- **WIFI_CONNECTED** — WiFi available; sync over HTTPS every 30 s
- **IDLE** — quiet, waiting for events; LoRa ping every 5 min

---

## Project structure

```
src/
  main.cpp          State machine + Arduino entry points
  frame.cpp/.h      LoRa frame encode/decode (shared with server)
  provisioning.cpp/.h  BLE first-boot wizard
  lorawan.cpp/.h    LMIC OTAA join + uplink/downlink
  wifi_sync.cpp/.h  WiFi HTTPS sync with Glustick server
  display.cpp/.h    OLED UI (U8g2)
  message_queue.cpp/.h  NVS-backed message queue
include/
  config.h          Pin mapping, constants, state enum
```

---

## Related

- [Glustick Family server](https://github.com/tkhemraj/glustick-family) — the Go backend these devices talk to
- [ChirpStack](https://www.chirpstack.io/) — self-hosted LoRaWAN network server
