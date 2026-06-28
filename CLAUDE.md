# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

PlatformIO firmware (ESP8266 / Wemos D1 mini, Arduino framework) that reads a
Wiener Netze smart meter over its optical IR head, CRC-checks and AES-128-CTR
decrypts the DLMS frame, parses the energy/power registers, and re-exports them as:

- a **Shelly EM** (Gen1 `SHEM`) over HTTP + CoIoT, for Home Assistant and other
  Shelly-API consumers (`lib/ShellyEM`), and/or
- an **MQTT** JSON state message (`lib/MeterMqtt`),

with telnet + serial logging (`lib/NetLog`) and OTA. Each output is toggled at
compile time in [include/config.h](include/config.h). Decrypt+parse logic is
ported from `aldadic/esp-smartmeter-reader`.

## Commands

```bash
pio run                 # build (default env: d1_mini)
pio run -t upload       # build + flash (USB first time; OTA after — see below)
pio device monitor      # local UART log @ 115200 (needs the GPIO2→GPIO1 bridge)
pio run -t clean
```

There is no test suite; changes are verified on real hardware. Watch the running
device over WiFi with `telnet smartmeter2shelly.local` (port 23) — usually more
practical than serial (see UART section).

**First flash is USB; subsequent flashes can be OTA.** After the first USB
upload, switch to wireless by editing [platformio.ini](platformio.ini): comment
`upload_protocol = esptool` and uncomment the `espota` block (set `upload_port`
to the device IP / `smartmeter2shelly.local`, and `--auth` only if `OTA_PASSWORD`
is set). `ArduinoOTA.handle()` runs every `loop()`, so the device is always
OTA-updatable once on WiFi.

## Config: secrets.h (required) + config.h (toggles)

- **[include/secrets.h](include/secrets.h.example)** is gitignored and **required
  to compile**. Copy it from [include/secrets.h.example](include/secrets.h.example)
  and fill in: the 16-byte `KEY` (AES-128, per meter), WiFi SSID/pass, OTA
  hostname/password, and MQTT broker settings (all wired up).
- **[include/config.h](include/config.h)** is tracked and holds compile-time
  feature toggles: `ENABLE_SERIAL_LOG`, `ENABLE_TELNET_LOG`, `ENABLE_SHELLY_EM`,
  `ENABLE_MQTT`, plus `MQTT_PUBLISH_INTERVAL_MS`. Disabled features are `#if`'d
  out of `main.cpp` entirely (the lib isn't compiled in), so toggling off frees
  flash/RAM. Changing a toggle needs a re-flash (OTA is fine).

## UART architecture (the most error-prone part)

The ESP8266 has limited UARTs and this project deliberately remaps them:

- **Meter** is read on **UART0 after `Serial.swap()`** → UART0 moves to GPIO15
  (TX) / GPIO13 (RX), off the USB header. The IR head connects there
  (RX→GPIO13/D7, TX→GPIO15/D8), 9600 8N1.
- **Local debug log** uses **UART1 (`Serial1`, GPIO2/D4, TX-only)**, aliased
  `DBG_UART` in [src/main.cpp](src/main.cpp).
- The global `LOG` is **not** `Serial1` — it's a `NetLog` that tees to `Serial1`
  (when `ENABLE_SERIAL_LOG`) **and** a telnet client. `SmartMeter`/`ShellyEM`/
  `MeterMqtt` all take `&LOG` as their `Stream*`/`Print*` logger.
- To see the UART log over USB you must physically bridge **GPIO2 (D4) → GPIO1
  (TX)**; without it `pio device monitor` shows nothing (attach a USB-TTL adapter
  to GPIO2 instead). USB serial also de-enumerates once WiFi is active in a
  weak-signal spot — hence telnet is the practical log path.

So `Serial` in code = the meter, `LOG` = the human-readable log (UART + telnet).

## Output modules (`lib/`)

Each module is self-contained (its own `logf` helper, no cross-lib symbols) so
`ShellyEM`/`MeterMqtt` stay independently reusable. `main.cpp` owns the wiring:
on each valid frame it maps `MeterReading` → the module inputs and calls them.

- **`lib/NetLog`** — a write-only `Stream` (`NetLog : public Stream`). Constructor
  takes `Print* local` (pass `nullptr` to drop the serial tee) + a telnet port.
  `beginServer()` starts the telnet `WiFiServer` (called from `startOTA()` once
  WiFi is up); `handle()` accepts/drops viewers each loop. One viewer at a time.
- **`lib/ShellyEM`** — Gen1 `SHEM` emulation (1 bidirectional emeter channel).
  HTTP (`ESP8266WebServer` :80): `/shelly`, `/settings`, `/status`, `/emeter/0`,
  `/cit/d`, `/cit/s`, `/relay/0`, `/reboot`. CoIoT/CoAP (`WiFiUDP` :5683):
  answers `cit/d`/`cit/s` GETs and pushes status to multicast 224.0.1.187. API:
  `begin(host)` / `update(ShellyEMData)` / `handle()`. **Started lazily on the
  first valid frame** (`shelly.begin()` in `loop()`, not `startOTA()`) so it never
  serves a `0` energy reading that HA's `total_increasing` sensor reads as a meter
  reset. mDNS is owned by ArduinoOTA; ShellyEM only `setInstanceName`/`addService`.
- **`lib/MeterMqtt`** — `PubSubClient` publisher. `begin()` / `loop()` (throttled
  reconnect + LWT) / `publish(MeterReading)`. Publishes a retained JSON state to
  `MQTT_TOPIC` every `MQTT_PUBLISH_INTERVAL_MS`, plus `online`/`offline` on
  `<base>/availability`.

### ⚠️ Energy units differ per output (don't unify blindly)

- **Shelly EM** (CoIoT + HTTP): energy is fed in **Wh** — HA's Gen1 emeter energy
  sensor is native Wh (`ShellyEMData.energyWh` = `MeterReading.activeImportWh`
  directly, no scaling).
- **MQTT**: keys are OBIS-style and energy is **kWh/kvarh** (`activeImportWh /
  1000`), because the consuming HA MQTT `sensor`s are kWh and their
  `value_template: "{{ value_json['+A'] }}"` does **not** divide. Power stays W.
  Keys: `+A -A +R -R` (kWh/kvarh), `+P -P +Q -Q` (W/var), `time`. Renaming these
  keys or units breaks the user's existing HA MQTT sensors.

## Shelly EM ↔ Home Assistant gotchas (hard-won)

These were the actual blockers — see the project memory for the full list:

1. **Every CoIoT status message must carry the device-id option** (3332 = `SHEM#
   <MAC>#2`); aioshelly/HA reject otherwise. Status also sends 3412 validity +
   3420 serial (serial must change per message). CoAP option delta/length is
   hand-encoded (`coapAddOption`).
2. **`/cit/d` and `/cit/s` are also served over HTTP**, not just CoAP — aioshelly
   fetches them over HTTP at init (fw ≥ 1.10); CoAP is for live push. The sensor
   `"D"` names in `/cit/d` (`power`/`energy`/`energyReturned`/`voltage`) are what
   HA's `BLOCK_SENSORS` keys on.
3. **mDNS service instance name must be the full 12-hex MAC** and start with
   `shelly`: `shellyem-AABBCCDDEEFF`. HA's zeroconf parses the MAC from it.

## Meter brand framing

Frame length and header length differ per meter and are compile-time constants
selected by build flag in [platformio.ini](platformio.ini) `build_flags`,
defined in [lib/SmartMeter/SmartMeter.h](lib/SmartMeter/SmartMeter.h):

- Default / `-D METER_BRAND_LANDIS_GYR=1`: Landis+Gyr E450 / Iskraemeco AM550
  (message 105, header 14).
- `-D METER_BRAND_SIEMENS=1`: Siemens (message 125, header 16).

`SM_PAYLOAD_LENGTH` is derived from these. If frames CRC-fail on a new meter,
the brand constants are the first suspect.

## SmartMeter parse pipeline

[lib/SmartMeter/SmartMeter.cpp](lib/SmartMeter/SmartMeter.cpp) — `poll()` is a
byte-at-a-time state machine driven from `loop()`:

1. Resync on HDLC start flag `0x7E`, confirmed by `0xA0` as the second byte.
2. Accumulate into `_buf` until `SM_MESSAGE_LENGTH`, then: `checkCrc()`
   (CRC16/X25 over bytes `[1 .. len-4]`, compared little-endian against the last
   two frame bytes) → `decrypt()` → `parse()`.
3. Returns `true` once per complete valid frame; a CRC or decrypt failure
   returns `false` and the loop resyncs.

**Decryption** is AES-128-CTR. The 16-byte CTR IV is assembled from the frame:
bytes `[HEADER .. HEADER+8)` (system title) + `[HEADER+10 .. HEADER+14)` (frame
counter) + zero padding + `iv[15]=0x02`; ciphertext starts at `HEADER+14`.
Platform-split: ESP8266 uses the `rweather/Crypto` `CTR<AES128>` library; ESP32
uses `mbedtls` (`#ifdef` branches both exist, but only ESP8266 is built today).

**Register parsing** (`bytesToInt`) addresses fields by **negative offset from
the end of the payload**, not from the start — offsets are hardcoded in
`parse()`. Energy registers are big-endian Wh/varh (divide by 1000 for
kWh/kvarh); power registers are already W/var.
