<div align="center">

# ◈ Glustick Link — Firmware

**Talk and text when the internet doesn't.**<br>
Off-grid comms firmware for the [LilyGo T-Deck](https://www.lilygo.cc/products/t-deck) —
ESP32-S3 · SX1262 LoRa · 2.8″ ST7789 · BlackBerry keyboard · mic + speaker.

<br>

[![build](https://img.shields.io/badge/build-7%2F7_envs_passing-4ade80?style=flat-square&logo=platformio&logoColor=white)](#build-matrix)&nbsp;
[![voice](https://img.shields.io/badge/voice-Codec2_over_LoRa-e11d48?style=flat-square)](#-voice-over-lora)&nbsp;
[![mcu](https://img.shields.io/badge/MCU-ESP32--S3-f5822a?style=flat-square&logo=espressif&logoColor=white)](#hardware)&nbsp;
[![radio](https://img.shields.io/badge/radio-SX1262-7c3aed?style=flat-square)](#hardware)&nbsp;
[![flash](https://img.shields.io/badge/voice_build-689KB_flash_·_60KB_RAM-4f90f0?style=flat-square)](#build-matrix)

<br>

*No cell tower. No Wi-Fi. No company in the middle. Two radios and open air.*<br>
*Part of the [Glustick Family](https://github.com/tkhemraj/glustick-family) self-hosted stack.*

</div>

---

## Why

Hand a kid a device, hand their friend another, and 3 km apart — phones off, internet
off — they can still reach each other. That's the whole product. Glustick Link turns a
pocket LoRa handheld into a **self-owned walkie-talkie and texter** that depends on
nothing but the two radios in your hands (and, optionally, a cheap repeater to stretch
the range). No SIM, no account, no cloud. You own every byte and every hop.

---

## What it does

| | Feature | How |
|-|---------|-----|
| 💬 | **Text, device-to-device** | Direct LoRa P2P — type on the BlackBerry keyboard, hit send. No server, no gateway. Pair two units with a shared key. |
| 🎙️ | **Voice over LoRa** *(new)* | Hold the trackball to talk. Speech is [Codec2](https://github.com/drowe67/codec2)-compressed to ~700 bps and streamed over the same radio. A walkie-talkie with no tower. |
| 📡 | **Dumb repeater** | Flash a spare Heltec board as a store-and-forward relay. It never sees plaintext (links are end-to-end encrypted) — it just extends range, for text **and** voice. |
| 🛰️ | **Meshtastic mode** | Join the public Meshtastic mesh + optional Wi-Fi/MQTT bridge and community map. |
| 🗺️ | **Offline maps + GPS** | Optional build with an SD-card tile map and live position. |
| 🔋 | **Runs for days** | ESP32-S3 + LiPo, battery gauge on-screen. |

```
┌──────────────────────────────────────────────┐
│ ◈ Glustick Link            RSSI -92  ▓▓▓ 86%  │
├──────────────────────────────────────────────┤
│  Dad     can you hear me?                      │
│  Me      loud and clear 🎙️                     │
│  Dad     [voice 0:03] ▶                        │
│                                                │
│  ┌──────────────────────────────────────────┐ │
│  │  hold ●trackball to talk · type to text   │ │
│  └──────────────────────────────────────────┘ │
└──────────────────────────────────────────────┘
```

---

## Build matrix

Install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html), then
`pio run -e <env> -t upload`. **`elysian` is the default** (plain T-Deck texting);
**`elysian_voice`** adds push-to-talk.

| env | board | what you get |
|-----|-------|--------------|
| **`elysian`** ⭐ | T-Deck | Direct P2P text, no server/gateway. The default. |
| **`elysian_voice`** 🎙️ | T-Deck | Everything in `elysian` **+ push-to-talk voice over LoRa**. |
| `glustic_fun` | T-Deck | P2P text + GPS + offline SD-card tile map. |
| `remotest` | T-Deck | Meshtastic node + Wi-Fi/MQTT bridge + community map. |
| `heltec_repeater` | Heltec v3 | Store-and-forward range extender (text + voice). |
| `heltec_v3` | Heltec v3 | Legacy LoRaWAN/OTAA pager (the original infrastructure path). |
| `tdeck` | T-Deck | LoRaWAN/OTAA on the T-Deck. |

Each has an **`_eu868`** sibling for Europe (868 MHz); US915 is the default. `remotest`
also has `_forever` / `_remote` named variants.

```bash
pio run -e elysian_voice -t upload   # flash two boards
pio device monitor                    # 115200 baud
```

### Name a device at flash time

Give each board a human name instead of the auto `Dev-A1B2`. It shows on-screen
and labels the messages it sends:

```bash
PLATFORMIO_BUILD_FLAGS='-D DEVICE_NAME=\"Zoe\"' pio run -e elysian_voice -t upload
```

(or add `-D DEVICE_NAME='"Zoe"'` to the env's `build_flags` in `platformio.ini`.)

### Wi-Fi memory (Meshtastic/`remotest` build)

The device **remembers every Wi-Fi it has ever joined** (up to 8) and, on boot,
reconnects to the most-recently-used network that's actually in range — then falls
back through the rest by recency. A network is **never forgotten unless you delete
it.** Manage the list over the USB serial monitor:

```
wifi                 # list remembered networks, most-recent first
wifi del 2           # forget network #2 from the list
wifi forget HomeAP   # forget by name
```

---

## 🎙️ Voice over LoRa

Press-to-talk voice on a link that normally carries a few hundred bits per second. The
trick is [**Codec2**](https://github.com/drowe67/codec2), David Rowe's open vocoder built
for exactly this — HF/VHF digital radio at **700 bps**.

**How to use it:** build `elysian_voice`, pair two T-Decks (same key), **hold the trackball
click to talk, release to listen.** Half-duplex, like any walkie-talkie.

**The design, and the one rule it follows — protect the range:**

| Choice | Value | Why |
|--------|-------|-----|
| Codec | Codec2 **700C** (700 bps) | ~4 bytes per 40 ms frame — tiny enough to stay real-time on a long-range PHY. |
| PHY | **SF8 / BW125** | A voice packet's airtime stays under the audio it carries (no stutter), while holding long range. |
| Framing | 9 frames/packet (~360 ms) | Rides the existing P2P packet as a new `FLAG_VOICE` type. |
| PTT | hold **GPIO0** (trackball click) | Configurable: `-D PTT_PIN=`. |

Want fidelity over reach? Rebuild with `VOICE_CODEC2_MODE=CODEC2_MODE_1300` (or `_3200`).
Want maximum reach? Raise `VOICE_SF`. **Voice also rebroadcasts through a `heltec_repeater`
unchanged**, so a cheap relay between two people extends voice range the same as text.

> **Status:** the full pipeline (mic → Codec2 → radio → Codec2 → speaker, dual-core,
> jitter-buffered) **compiles and links** on the real toolchain. On-air audio tuning
> (mic gain, speaker level, real SF8 range) is a bench step with two boards.
>
> **Regulatory:** continuous PTT holds one channel; on US915 ISM that can exceed FCC
> Part 15 dwell limits. Treat voice as experimental / amateur-band use and keep bursts short.

---

## Repeater

A spare **Heltec WiFi LoRa 32 V3** (no keyboard, so it can't be an endpoint) becomes a
store-and-forward relay:

```bash
pio run -e heltec_repeater -t upload
```

It shares the `elysian` radio (915 MHz, SF10), **learns the family's net from the first
packet it hears**, de-duplicates by `(sender, msg-id)` so a packet is never aired twice,
and staggers its retransmit so two repeaters don't key up on the same millisecond. Because
Glustick links are end-to-end encrypted, the repeater **never sees plaintext** — it only
moves range around. Hold PRG at boot to forget the learned net. Europe: `heltec_repeater_eu868`.

---

## Hardware

**Shipping unit: [LilyGo T-Deck](https://www.lilygo.cc/products/t-deck).** The `heltec_*`
envs target the original Heltec WiFi LoRa 32 V3 (legacy endpoint + the repeater role).

| Component | Details |
|---|---|
| MCU | ESP32-S3 (`T-Deck`, 16 MB flash / 8 MB OPI PSRAM) |
| Radio | SX1262 — LoRa 868 / 915 MHz (CS 9 · SCK 40 · MISO 38 · MOSI 41 · BUSY 13 · RST 17 · DIO1 45) |
| Display | ST7789 320×240 IPS, backlight GPIO 42 |
| Input | I²C BlackBerry keyboard (0x55, INT 46) + trackball (GPIO 1/2/3/15, click = GPIO 0) |
| Audio | **Speaker** via I²S amp (WS 5 · BCK 7 · DOUT 6) · **Mic** ES7210 I²S ADC (MCLK 48 · SCK 47 · LRCK 21 · DIN 14) |
| Connectivity | Wi-Fi 2.4 GHz + BLE 5.0 + LoRa (P2P / Meshtastic / LoRaWAN) |
| Power | USB-C + LiPo, peripheral power latch GPIO 10 · battery ADC GPIO 4 |

---

## Transports, at a glance

| Path | Env | Server? | Range | Best for |
|------|-----|---------|-------|----------|
| **Direct P2P** | `elysian` / `elysian_voice` | none | ~5–15 km (SF10 text) / long (SF8 voice) | Two-to-a-few people, fully off-grid |
| **+ Repeater** | add `heltec_repeater` | none | extends P2P | Stretching P2P past line-of-sight |
| **Meshtastic** | `remotest` | optional MQTT | mesh | Joining the wider community mesh |
| **LoRaWAN** | `heltec_v3` / `tdeck` | gateway + Glustick Family | gateway-dependent | Fixed infrastructure, Wi-Fi fallback |

The LoRaWAN path keeps the original first-boot BLE/QR provisioning wizard and the
chunked frame protocol that matches the [Glustick Family](https://github.com/tkhemraj/glustick-family)
server — see [`docs`](#provisioning-lorawan-path) below.

---

## Frame format (P2P)

```
NET_ID[4] │ FROM_ID[4] │ MSG_ID[2] │ FLAGS[1] │ BODY_LEN[1] │ BODY[0..38]
```

`FLAGS`: `0x00` text · `0x01` ping · `0x02` voice (raw Codec2, len 0 = end-of-talk).
Peers on the same `NET_ID` (derived from the shared key) hear each other; a repeater
rebroadcasts any flag it doesn't recognize as pairing traffic.

<details>
<summary><strong>LoRaWAN path — provisioning, BLE GATT, chunked frames</strong></summary>

The `heltec_v3` / `tdeck` LoRaWAN builds boot into a BLE + QR provisioning wizard, join via
OTAA, and sync to the Glustick Family server over Wi-Fi when home. First boot derives a
DevEUI from the eFuse MAC (IEEE EUI-64), generates a random AppKey (`esp_fill_random`),
and shows a QR + 6-digit pairing PIN. The app negotiates a 512-byte MTU, completes BLE
Secure Connections pairing, then writes 10 GATT characteristics (DevEUI, AppEUI, AppKey,
server URL, parent token, Wi-Fi SSID/pass, kid name, commit, TLS cert). QR format:
`GSF:{devEUI}:{appEUI}:{appKey}`. The chunked LoRaWAN frame (50-byte max, 44-byte payload,
up to 8 chunks / 352-byte message) matches `apps/api/internal/lora/chunker.go` on the
server. Wi-Fi sync POSTs `/api/v1/lora/uplink` and polls `/api/v1/lora/downlink` every 30 s
over TLS (cert-pinned when a PEM is provisioned, else `setInsecure()`).

</details>

---

## Project structure

```
src/
├── main.cpp                setup()/loop(), state machine, input routing
├── radio_p2p.cpp/.h        SX1262 P2P: text, ping, FLAG_VOICE, repeater relay
├── voice.cpp/.h            PTT voice: core-0 audio task, Codec2, cross-core queues
├── voice_jbuf.cpp/.h       lock-free SPSC RX jitter buffer
├── audio.cpp/.h            I2S speaker + 8 kHz ES7210 mic
├── repeater_main.cpp       store-and-forward relay entry (ROLE_REPEATER)
├── keyboard / trackball / touch / display   T-Deck UI
├── radio_meshtastic + wifi_mesh + gps + map_tiles   Meshtastic / GPS / maps
├── lorawan + provisioning + wifi_sync + message_queue   LoRaWAN infra path
└── p2p_pair.cpp/.h         direct-pairing handshake
lib/es7210/                 vendored LilyGo ES7210 mic driver
platformio.ini              12 build envs (see the matrix above)
```

---

## Related

| Project | What |
|---|---|
| [Glustick Family](https://github.com/tkhemraj/glustick-family) | The self-hosted server + app this talks to |
| [Codec2](https://github.com/drowe67/codec2) | The low-bitrate speech codec behind voice-over-LoRa |
| [RadioLib](https://github.com/jgromes/RadioLib) | SX1262 driver for the P2P + voice path |
| [Meshtastic](https://meshtastic.org/) | The community mesh `remotest` joins |
| [LilyGo T-Deck](https://www.lilygo.cc/products/t-deck) | The hardware |
