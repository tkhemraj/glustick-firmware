<div align="center">

# Glustick Link — Firmware

**Messaging that works when the internet doesn't.**<br>
LoRaWAN + WiFi firmware for the [Heltec WiFi LoRa 32 V3](https://heltec.org/project/wifi-lora-32-v3/) · ESP32-S3 + SX1262 + SSD1306 OLED

<br>

[![Build](https://img.shields.io/badge/build-passing-4ade80?style=flat-square&logo=platformio&logoColor=white)](#building)&nbsp;
[![Flash](https://img.shields.io/badge/flash-1.52%20MB%20%2845%25%29-4f90f0?style=flat-square)](#building)&nbsp;
[![RAM](https://img.shields.io/badge/RAM-82%20KB%20%2825%25%29-4f90f0?style=flat-square)](#building)&nbsp;
[![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32--S3-f5822a?style=flat-square&logo=platformio&logoColor=white)](https://platformio.org)&nbsp;
[![LoRaWAN](https://img.shields.io/badge/LoRaWAN-OTAA-7c3aed?style=flat-square)](#how-it-works)

<br>

*Part of the [Glustick Family](https://github.com/tkhemraj/glustick-family) self-hosted communications stack.*

</div>

---

## Which path are you on?

Two ways to use Glustick Link hardware. **Most families should start with Meshtastic.**

<table>
<tr>
<td valign="top" width="50%">

### ✅ Path 1 — Meshtastic
**Recommended · No custom firmware needed**

Buy two Heltec boards, flash Meshtastic, done. The Glustick Family server speaks the Meshtastic serial protocol natively — no gateway, no ChirpStack, no infrastructure.

**Steps**

1. Buy two [Heltec WiFi LoRa 32 V3](https://heltec.org/project/wifi-lora-32-v3/) boards
2. Flash [Meshtastic firmware](https://meshtastic.org/docs/getting-started/flashing-firmware/) via their web flasher
3. Set `MESHTASTIC_PORT=/dev/ttyACM0` in your Glustick Family `.env`
4. Register the kid's board by entering its 8-character node ID (shown on OLED as `!DEADBEEF`) in the app

**No gateway &nbsp;·&nbsp; No ChirpStack &nbsp;·&nbsp; Works immediately**

</td>
<td valign="top" width="50%">

### 🛠 Path 2 — Custom Firmware
**This repo · LoRaWAN + WiFi fallback**

OTAA join to a LoRaWAN gateway. Uplinks route through ChirpStack → MQTT → Glustick Family server. Longer range via proper gateway infrastructure, plus WiFi sync when the device is home.

**Steps**

1. Set up a LoRaWAN gateway + ChirpStack v4
2. Build and flash this firmware with PlatformIO
3. Provision with the Glustick Family app via QR code or BLE
4. Enable the `lora` Docker Compose profile on your server

**Longer range &nbsp;·&nbsp; WiFi fallback &nbsp;·&nbsp; Full control**

</td>
</tr>
</table>

---

## Hardware

| Component | Details |
|---|---|
| MCU | ESP32-S3 (`heltec_wifi_lora_32_V3`) |
| Radio | SX1262 — LoRa 868 / 915 MHz |
| Display | SSD1306 128×64 OLED (I²C, hardware reset GPIO 21) |
| Connectivity | WiFi 2.4 GHz + BLE 5.0 + LoRaWAN |
| Power | USB-C + LiPo header |
| Battery ADC | GPIO 1 (voltage divider, 3.0–4.2 V → 0–100%) |

<details>
<summary><strong>Pin mapping</strong></summary>

| Signal | GPIO |
|---|---|
| LMIC NSS (SPI CS) | 8 |
| LMIC RST | 12 |
| LMIC DIO1 | 14 |
| OLED SDA | 17 |
| OLED SCL | 18 |
| OLED RST | 21 |
| VBAT ADC | 1 |

</details>

---

## Building

Install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html) or the [VS Code extension](https://platformio.org/install/ide?install=vscode).

**US915 — North America (default)**

```bash
# Install dependencies and build
pio run -e heltec_v3

# Flash to a connected device
pio run -e heltec_v3 --target upload

# Serial monitor (115200 baud)
pio device monitor
```

**EU868 — Europe**

```bash
pio run -e heltec_v3_eu868
pio run -e heltec_v3_eu868 --target upload
```

> [!NOTE]
> The pre-build script `scripts/patch_lmic.py` automatically configures MCCI LMIC for the SX1262 radio before every compile. It also resolves a symbol conflict between LMIC's `hal_init` and ESP32's `libpp.a` via `-Wl,--allow-multiple-definition`. No manual library edits required.

### Dependencies

| Library | Version | Purpose |
|---|---|---|
| `mcci-catena/MCCI LoRaWAN LMIC library` | `^4.1.1` | OTAA LoRaWAN join + uplink/downlink |
| `bblanchon/ArduinoJson` | `^7.0.0` | WiFi sync JSON parsing |
| `olikraus/U8g2` | `^2.35.9` | SSD1306 OLED display driver |
| `ricmoo/QRCode` | `^0.0.1` | QR code rendering during provisioning |

---

## First boot — provisioning

On first boot the device derives its identity from the chip eFuse MAC, generates a random AppKey, displays a QR code and pairing PIN on the OLED, and waits up to **5 minutes** for the Glustick Family mobile app.

```
┌──────────────────────────────────────┐
│ ▓▓▓▓▓▓▓▓▓▓▓▓   SCAN QR              │
│ ▓         ▓▓                        │
│ ▓  ██████ ▓▓   BLE:                 │
│ ▓  ██████ ▓▓   A1B2                 │
│ ▓         ▓▓   PIN:                 │
│ ▓▓▓▓▓▓▓▓▓▓▓▓   847291               │
└──────────────────────────────────────┘
  Device OLED during provisioning
```

**Provisioning flow**

1. **Power on.** The device derives a DevEUI from the chip eFuse MAC using the IEEE EUI-64 rule and generates a cryptographically random AppKey (`esp_fill_random`). The QR code, BLE name suffix, and a random 6-digit pairing PIN appear on the OLED.

2. **Scan or connect via BLE.** Open the Glustick Family app → <kbd>Settings</kbd> → <kbd>Glustick Link</kbd> → <kbd>Add device</kbd>. Scan the QR to pre-fill credentials, or tap <kbd>Set up via Bluetooth</kbd> and connect to `GsfLink-XXXX`.

3. **Enter the pairing PIN.** When your phone's Bluetooth pairing dialog appears, enter the 6-digit PIN shown on the OLED. All characteristic reads and writes are rejected until the encrypted authenticated connection is established.

4. **App writes credentials via GATT.** Ten characteristics are written in sequence (see table below). Values are staged in RAM until commit — an interrupted write leaves NVS untouched.

5. **Device reboots.** BLE advertising stops. The device joins LoRaWAN (up to 2 minutes) and connects to WiFi if credentials were provided.

> [!WARNING]
> Provisioning times out after **5 minutes** if the app does not connect. Power cycle to retry.

### QR code format

```
GSF:{devEUI}:{appEUI}:{appKey}
```

Example: `GSF:A8610A34567BFFFE:0000000000000000:A8610A34567BFFFE5A5A5A5A5A5A5A5A`

Version-3 alphanumeric QR, ECC\_LOW. Rendered at 2×2 px per module into the left 64×64 panel (3 px margin). Case-insensitive — the app accepts both `GSF:` and `gsf:`.

### DevEUI derivation

`provisioning_derive_defaults()` derives the DevEUI from the chip's 48-bit eFuse MAC using the IEEE EUI-64 rule: `{mac[0]^0x02}:{mac[1]}:{mac[2]}:FF:FE:{mac[3]}:{mac[4]}:{mac[5]}`, where `mac[0]` is the most-significant (OUI) byte. The result is deterministic and unique per chip.

The AppKey is generated with `esp_fill_random()` — it is never derivable from public device information. The QR code carries the generated AppKey so the app can pre-fill it; the BLE characteristics allow it to be replaced with a network-issued key.

### BLE GATT characteristics

The app negotiates a 512-byte MTU and must complete BLE Secure Connections pairing (PIN displayed on OLED) before any characteristic access is permitted. It then writes these 10 characteristics in sequence:

| UUID (suffix) | Value written |
|---|---|
| `…-000000000001` | DevEUI — 16 hex chars |
| `…-000000000002` | AppEUI — 16 hex chars |
| `…-000000000003` | AppKey — 32 hex chars |
| `…-000000000004` | Glustick server URL |
| `…-000000000005` | Parent PASETO token |
| `…-000000000006` | WiFi SSID (optional) |
| `…-000000000007` | WiFi password (optional) |
| `…-000000000008` | Kid's name (shown on OLED) |
| `…-000000000009` | Commit — write `"1"` to persist all staged values |
| `…-00000000000a` | Server TLS cert — PEM string for `setCACert()` (optional) |

Full UUID prefix: `12345678-1234-1234-1234-`

> [!NOTE]
> The server TLS cert characteristic (`…000a`) is optional but strongly recommended. When provisioned, the device pins TLS connections to that certificate using `setCACert()`. When absent, connections fall back to `setInsecure()`. To get your server's PEM: `openssl s_client -connect yourserver:443 </dev/null 2>/dev/null | openssl x509 -outform PEM`

---

## How it works

### State machine

```
PROVISIONING ──→ JOINING ──→ LORA_CONNECTED
                                    ↕
                          WIFI_CONNECTED ↔ IDLE
```

| State | Description |
|---|---|
| `PROVISIONING` | BLE advertising active. OLED shows QR code and BLE name suffix. |
| `JOINING` | LMIC OTAA join in progress. Can take up to 2 minutes. |
| `LORA_CONNECTED` | LoRaWAN joined, no WiFi. Outbound messages sent over LoRa. 5-minute keepalive ping. |
| `WIFI_CONNECTED` | WiFi available. Messages synced over HTTPS every 30 s. LoRaWAN stays active as fallback. |
| `IDLE` | Quiet. OLED refreshes every 30 s: kid name, RSSI (dBm), battery %, queued message count. |
| `ERROR` | Unrecoverable failure. OLED shows error message. Power cycle required. |

> When both WiFi and LoRa are available, outbound messages go over WiFi (faster, lower power). LoRa is used when WiFi is absent. Inbound messages arrive via both transports independently.

### Frame format

Matches `apps/api/internal/lora/chunker.go` in the server repo exactly. Both sides encode and decode the same structure.

```
┌─────────┬─────────┬───────────────┬───────────┬─────────────┬──────────────────┐
│ Byte 0  │ Byte 1  │   Bytes 2–3   │  Byte 4   │   Byte 5    │   Bytes 6–49     │
├─────────┼─────────┼───────────────┼───────────┼─────────────┼──────────────────┤
│ version │  type   │     msgID     │ chunkIdx  │ chunkTotal  │    payload       │
│  = 1    │0·1·2    │  uint16 LE    │  0-based  │ total count │  up to 44 bytes  │
└─────────┴─────────┴───────────────┴───────────┴─────────────┴──────────────────┘
 type: 0 = message · 1 = ack · 2 = ping
```

| Constant | Value |
|---|---|
| Max frame size | 50 bytes |
| Max payload per chunk | 44 bytes |
| Max chunks per message | 8 |
| Max message body | 352 bytes |

Messages of 44 bytes or fewer fit in a single frame. Longer messages are split into up to 8 chunks. The server reassembles them using Redis with a 2-minute TTL per chunk. Message IDs are stored in NVS and survive reboots.

### WiFi sync

The device POSTs outbound messages and polls for inbound messages every **30 seconds**:

| Method | Endpoint | Purpose |
|---|---|---|
| `POST` | `/api/v1/lora/uplink` | Send outbound message — body JSON, `Authorization: Bearer` header |
| `GET` | `/api/v1/lora/downlink` | Poll for inbound messages — returns JSON array |

TLS is used for all requests. If a server PEM cert was provisioned via BLE characteristic `…000a`, the device calls `setCACert()` to pin connections to that certificate. If no cert was provisioned, `setInsecure()` is used as a fallback — see the provisioning section above for how to set this up. Messages that fail to send over WiFi are left in the NVS queue and retried on the next cycle, or sent over LoRa if WiFi drops.

### OLED display states

| State | Display |
|---|---|
| Boot | `Glustick Link` + firmware version |
| Provisioning | QR code (left half) + BLE name suffix and 6-digit pairing PIN (right half) |
| Joining | `Joining LoRaWAN… This can take up to 2 minutes.` |
| Idle / LoRa connected | Kid name, RSSI (dBm), battery bar + %, queued message badge |
| Sending | `Transmitting…` + queued count |
| WiFi sync | `WiFi connected. Syncing messages…` |
| Incoming message | Sender label + body (word-wrapped at 21 chars/line) |
| Error | `ERROR` header + wrapped message text |

---

## Project structure

```
src/
├── main.cpp              State machine · Arduino setup() / loop()
├── frame.cpp / .h        LoRa frame encode/decode — matches server exactly
├── provisioning.cpp / .h BLE GATT first-boot wizard + NVS persistence
├── lorawan.cpp / .h      LMIC OTAA join, uplink, downlink, ping
├── wifi_sync.cpp / .h    WiFi HTTPS POST/GET sync with Glustick server
├── display.cpp / .h      OLED UI (U8g2) — all display states + QR rendering
└── message_queue.cpp / .h NVS-backed outbound message queue (survives reboots)
include/
└── config.h              Pin mapping, NVS keys, timeouts, frame constants
scripts/
└── patch_lmic.py         Pre-build: patches MCCI LMIC project_config for SX1262
platformio.ini            Two envs: heltec_v3 (US915) · heltec_v3_eu868 (EU868)
```

---

## Related

| Project | Description |
|---|---|
| [Glustick Family](https://github.com/tkhemraj/glustick-family) | Go backend these devices talk to — routing, content filtering, delivery pipeline |
| [Meshtastic](https://meshtastic.org/) | Recommended no-firmware-needed alternative transport |
| [ChirpStack](https://www.chirpstack.io/) | Self-hosted LoRaWAN network server for the infrastructure path |
| [MCCI LMIC](https://github.com/mcci-catena/arduino-lmic) | LoRaWAN stack for Arduino — OTAA join, Class A uplink/downlink, SX1262 HAL |
| [Heltec WiFi LoRa 32 V3](https://heltec.org/project/wifi-lora-32-v3/) | The hardware these devices run on |
