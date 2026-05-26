# Glustick Link — Firmware

Firmware for the **Glustick Link** off-grid messaging device. Built with [PlatformIO](https://platformio.org/) targeting the **Heltec WiFi LoRa 32 V3** (ESP32-S3 + SX1262 + SSD1306 OLED).

Glustick Link lets families communicate when the internet is out. Messages travel over LoRaWAN to a gateway, through ChirpStack, and into the Glustick Family server. When home WiFi is available the device syncs faster over HTTPS and falls back to LoRa automatically.

**Build status:** builds clean at 1.52 MB flash (45%) and 82 KB RAM (25%).

---

## Hardware

| Component | Details |
|---|---|
| MCU | ESP32-S3 (Heltec WiFi LoRa 32 V3 — `heltec_wifi_lora_32_V3`) |
| Radio | SX1262 (LoRa 868/915 MHz) |
| Display | SSD1306 128×64 OLED (I²C, hardware reset on GPIO 21) |
| Connectivity | WiFi 2.4 GHz + BLE 5.0 + LoRaWAN |
| Power | USB-C + LiPo header; battery percentage read from GPIO 1 ADC |

---

## Building

Install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html) or the [VS Code extension](https://platformio.org/install/ide?install=vscode).

```bash
# Install dependencies and build (US915 — North America, default)
pio run -e heltec_v3

# EU868 (Europe)
pio run -e heltec_v3_eu868

# Flash to a connected device
pio run -e heltec_v3 --target upload

# Open the serial monitor (115200 baud)
pio device monitor
```

### LMIC patch script

MCCI LMIC's bundled `project_config/lmic_project_config.h` defaults to SX1276. The pre-build script `scripts/patch_lmic.py` overwrites that file with:

```c
#define CFG_sx1262_radio 1
#define LMIC_USE_INTERRUPTS
```

This runs automatically before every compile — you do not need to edit anything manually. The script is idempotent: it only writes if the content has changed, so incremental builds are not affected.

The build also passes `-Wl,--allow-multiple-definition` to resolve a symbol conflict between LMIC's `hal_init` and ESP32's `libpp.a`. LMIC's version is linked first and is the one used for SX1262 SPI initialisation.

### Dependencies (managed by PlatformIO)

| Library | Version | Purpose |
|---|---|---|
| `mcci-catena/MCCI LoRaWAN LMIC library` | ^4.1.1 | OTAA LoRaWAN join + uplink/downlink |
| `bblanchon/ArduinoJson` | ^7.0.0 | WiFi sync JSON parsing |
| `olikraus/U8g2` | ^2.35.9 | OLED display driver |
| `ricmoo/QRCode` | ^0.0.1 | QR code rendering during provisioning |

---

## First boot — provisioning

On first boot the device derives its identity from the chip MAC address, displays a QR code and BLE name on the OLED, and waits up to **5 minutes** for the mobile app to connect.

### What the OLED shows

The left half of the 128×64 display shows a QR code. The right half shows:

```
SCAN TO
REGISTER

OR BLE:
A1B2
```

The four hex characters are the last 4 of the chip ID — they also appear in the BLE device name (`GsfLink-A1B2`). Scan the QR code with the Glustick Family app to pre-fill registration, or tap "Add device" and connect via Bluetooth.

### QR code format

```
GSF:{devEUI}:{appEUI}:{appKey}
```

Example: `GSF:A8610A34567BFFFE:0000000000000000:A8610A34567BFFFE5A5A5A5A5A5A5A5A`

The app accepts both `gsf:` and `GSF:` (case-insensitive). Scanning fills the DevEUI, AppEUI, and AppKey fields automatically.

The QR is a version-3 alphanumeric code with ECC_LOW, rendered at 2×2 pixels per module into the 64×64 left panel (3 px margin on each side).

### DevEUI derivation

`provisioning_derive_defaults()` runs before BLE advertising begins. It derives the DevEUI from the chip's 48-bit eFuse MAC using the IEEE EUI-64 rule: `{mac[0]^0x02}:{mac[1]}:{mac[2]}:FF:FE:{mac[3]}:{mac[4]}:{mac[5]}`. The result is deterministic and unique per chip.

A factory-default AppKey is also derived deterministically from the chip ID (each byte XOR'd with `0x5A`). This is a convenience default for development — production units should use factory-burned keys registered in ChirpStack.

### BLE provisioning flow

1. Open the Glustick Family mobile app
2. Go to Settings → Glustick Link → Add device → Set up via Bluetooth
3. The app scans for BLE devices named `GsfLink-XXXX`
4. On connect the app negotiates a 512-byte MTU and writes 8 GATT characteristics in sequence:

| Characteristic UUID | Value written |
|---|---|
| `…000000000001` | DevEUI (16 hex chars) |
| `…000000000002` | AppEUI (16 hex chars) |
| `…000000000003` | AppKey (32 hex chars) |
| `…000000000004` | Glustick server URL |
| `…000000000005` | Parent PASETO token |
| `…000000000006` | WiFi SSID (optional) |
| `…000000000007` | WiFi password (optional) |
| `…000000000008` | Kid's name (shown on OLED) |

5. The app writes `1` to the commit characteristic (`…000000000009`)
6. The firmware validates that DevEUI, AppEUI, and AppKey are present and the correct length, then persists all values to NVS (ESP32 non-volatile storage)
7. BLE advertising stops and the device reboots into normal operation

Provisioning times out after 5 minutes if the app does not connect. Power cycle to retry.

Values are staged in RAM until commit; an incomplete write (app disconnects mid-flow) does not corrupt NVS.

---

## Frame format

Matches `apps/api/internal/lora/chunker.go` in the server repo exactly. Both sides encode and decode the same structure.

```
Byte 0:   version    = 1
Byte 1:   type       = 0 (message) | 1 (ack) | 2 (ping)
Byte 2-3: msgID      uint16 little-endian
Byte 4:   chunkIdx   0-based
Byte 5:   chunkTotal total chunk count for this message
Byte 6+:  payload    up to 44 bytes
```

Messages of 44 bytes or fewer fit in a single frame. Longer messages are split into multiple frames; the server reassembles them using Redis with a 2-minute TTL per chunk. The firmware assigns message IDs from a counter stored in NVS so they survive reboots.

---

## State machine

```
PROVISIONING  ──→  JOINING  ──→  LORA_CONNECTED
                                       ↕
                             WIFI_CONNECTED  ←──→  IDLE
```

| State | Description |
|---|---|
| `PROVISIONING` | BLE advertising; OLED shows QR code and BLE name |
| `JOINING` | LMIC OTAA join in progress; can take up to 2 minutes |
| `LORA_CONNECTED` | Joined, no WiFi; outbound messages sent over LoRa; LoRa ping every 5 min |
| `WIFI_CONNECTED` | WiFi available; messages synced over HTTPS every 30 s; LoRaWAN still active as fallback |
| `IDLE` | Quiet; OLED refreshes every 30 s showing kid name, RSSI, battery %, queued message count |
| `ERROR` | Unrecoverable failure; OLED shows error text; power cycle required |

When both WiFi and LoRa are available, outbound messages go over WiFi (faster, lower power). LoRa is used when WiFi is absent. Inbound messages arrive via both transports independently.

---

## WiFi sync

The device posts outbound messages to `{serverUrl}/api/v1/lora/uplink` and polls `{serverUrl}/api/v1/lora/downlink` every 30 seconds. Both requests carry the parent PASETO token in an `Authorization: Bearer` header. TLS is used; self-signed certificates are accepted (`setInsecure()`), which is appropriate for a family-owned server.

Messages that fail to send over WiFi are left in the NVS queue and retried on the next sync cycle or sent over LoRa if WiFi drops.

---

## OLED display states

| State | What's shown |
|---|---|
| Boot | "Glustick Link" + firmware version |
| Provisioning | QR code (left) + BLE name suffix (right) |
| Joining | "Joining LoRaWAN… This can take up to 2 minutes." |
| Idle / LoRa connected | Kid name, RSSI (dBm), battery bar + %, queued message badge |
| Sending | "Transmitting…" + queued count |
| WiFi sync | "WiFi connected. Syncing messages…" |
| Incoming message | Sender label + body (word-wrapped at 21 chars/line) |
| Error | "ERROR" header + wrapped message text |

---

## Project structure

```
src/
  main.cpp              State machine + Arduino setup/loop
  frame.cpp / .h        LoRa frame encode/decode (matches server exactly)
  provisioning.cpp / .h BLE GATT first-boot wizard + NVS persistence
  lorawan.cpp / .h      LMIC OTAA join, uplink, downlink, ping
  wifi_sync.cpp / .h    WiFi HTTPS POST/GET sync with Glustick server
  display.cpp / .h      OLED UI (U8g2) — all display states + QR rendering
  message_queue.cpp / .h NVS-backed outbound message queue
include/
  config.h              Pin mapping (SPI/SDA/SCL/RST/DIO), NVS keys, timeouts
scripts/
  patch_lmic.py         Pre-build: patches MCCI LMIC's project_config for SX1262
platformio.ini          PlatformIO project config — two envs: heltec_v3, heltec_v3_eu868
```

---

## Related

- [Glustick Family server](https://github.com/tkhemraj/glustick-family) — the Go backend these devices talk to
- [ChirpStack](https://www.chirpstack.io/) — self-hosted LoRaWAN network server used as the cloud transport
- [MCCI LMIC](https://github.com/mcci-catena/arduino-lmic) — LoRaWAN stack for Arduino
