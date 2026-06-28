# WienerNetze SmartMeter2Shelly EM

PlatformIO firmware (ESP8266 / Wemos D1 mini) that reads a Wiener Netze smart
meter over its optical IR head, CRC-checks and AES-128-CTR decrypts the DLMS
frame, parses the energy/power registers, and exposes them as:

- a **Shelly EM** (Gen1 `SHEM`, HTTP + CoIoT) for Home Assistant and other
  Shelly-API consumers, and/or
- **MQTT** JSON state,

with **telnet + serial** debug logging and **OTA** updates. Each output is
switched on/off in [include/config.h](include/config.h).

Decrypt+parse logic is ported from
[aldadic/esp-smartmeter-reader](https://github.com/aldadic/esp-smartmeter-reader).

## Status

Working & hardware-verified: meter read/decrypt/parse, WiFi+OTA, telnet/serial
log, Shelly EM (HTTP + CoIoT, verified against `aioshelly`), and MQTT (verified;
drives existing HA MQTT sensors). **Open:** adding the Shelly EM device to the
live HA instance (the MQTT path is already feeding HA).

## Project layout

```
platformio.ini            [env:d1_mini] — ESP8266 / d1_mini / arduino
include/config.h          feature toggles (serial/telnet/Shelly EM/MQTT) — tracked
include/secrets.h         gitignored — KEY, WiFi, OTA, MQTT. Copy from secrets.h.example
include/secrets.h.example template
src/main.cpp              wires SmartMeter -> NetLog + ShellyEM + MeterMqtt
lib/SmartMeter/           frame read + CRC + AES-CTR decrypt + register parse
lib/NetLog/               write-only Stream teeing the log to UART + telnet
lib/ShellyEM/             Gen1 Shelly EM emulation (HTTP :80 + CoIoT/CoAP :5683)
lib/MeterMqtt/            MQTT publisher (PubSubClient)
```

## Setup

1. Copy `include/secrets.h.example` → `include/secrets.h` and fill in: the 16-byte
   `KEY` (AES-128 key from smartmeter-web.wienernetze.at, per meter), WiFi
   SSID/pass, OTA hostname/password, and MQTT broker settings.
2. (Optional) Toggle features in [include/config.h](include/config.h).
3. Build & upload: `pio run -t upload` (first flash over USB — see below).

## Feature toggles — `include/config.h`

Compile-time switches (edit, then re-flash; OTA is fine). Disabled features are
compiled out entirely, freeing flash/RAM.

| Toggle | Default | What it does |
|--------|---------|--------------|
| `ENABLE_SERIAL_LOG` | 1 | Local debug log on UART1 (Serial1 / GPIO2) |
| `ENABLE_TELNET_LOG` | 1 | Debug log over WiFi (telnet :23) |
| `ENABLE_SHELLY_EM`  | 1 | Shelly EM emulation (HTTP + CoIoT) for HA |
| `ENABLE_MQTT`       | 1 | Publish readings to MQTT (broker in secrets.h) |

`MQTT_PUBLISH_INTERVAL_MS` (default 3000) sets the publish cadence.

## Build / flash (USB first, OTA after)

```bash
pio run                 # build
pio run -t upload       # build + flash
```

The **first** flash is over USB. After that the device is always OTA-updatable
on WiFi (`ArduinoOTA.handle()` runs every loop). To upload wirelessly, edit
[platformio.ini](platformio.ini): comment `upload_protocol = esptool` and
uncomment the `espota` block (set `upload_port` to the IP /
`smartmeter2shelly.local`, and `--auth` only if `OTA_PASSWORD` is set).

## Wiring — D1 mini (ESP8266) + optical IR read head (9600 8N1)

| IR head | D1 mini        |
|---------|----------------|
| VCC     | 3V3            |
| GND     | GND            |
| RX      | GPIO13 (D7)    |
| TX      | GPIO15 (D8)    |

The meter UART is **UART0 after `Serial.swap()`** (UART0 → GPIO15 TX / GPIO13 RX),
which moves it off the USB header. Debug logging uses **UART1 (`Serial1`, GPIO2,
TX-only)**.

## Logs

USB serial becomes unreliable once the WiFi radio is active (the port
de-enumerates under the current spike in a weak-signal spot), so the log is teed
to a **telnet server on port 23** as well as the local UART (`lib/NetLog`).

```
telnet smartmeter2shelly.local      # or the device IP
```

One telnet viewer at a time; a new connection replaces a stale one.

> **To read the local UART over USB:** bridge **GPIO2 (D4) → GPIO1 (TX)** so
> `Serial1` is routed into the USB-serial chip (monitor at **115200**), or attach
> a USB-TTL adapter to GPIO2. With `ENABLE_SERIAL_LOG = 0` the UART tee is off.

## Shelly EM (Home Assistant)

With `ENABLE_SHELLY_EM`, the device emulates a Gen1 Shelly EM (`SHEM`, one
bidirectional emeter channel). Add it in HA via **Settings → Devices & Services**
— it should be auto-discovered via mDNS as `shellyem-<MAC>`, or add it manually
by IP. HA reads live values over CoIoT; the Energy Dashboard sees grid
import (`+A`) and return (`-A`).

The device comes online only after the first valid meter frame, so it never
advertises a `0` energy reading that HA's `total_increasing` sensor would
mistake for a meter reset.

## MQTT

With `ENABLE_MQTT`, a retained JSON state is published to `MQTT_TOPIC` (default
`homeassistant/sensor/smartmeter/state`) every `MQTT_PUBLISH_INTERVAL_MS`, plus
`online`/`offline` on `<base>/availability` (LWT). Keys are OBIS-style; energy
is **kWh/kvarh** (raw Wh ÷1000), power is **W/var**:

```json
{"time":"2026-06-29T01:01:05",
 "+A":3378.415,"-A":126.528,"+R":7.979,"-R":2406.598,
 "+P":538,"-P":0,"+Q":0,"-Q":255}
```

Example HA sensor (no division — the payload is already kWh):

```yaml
mqtt:
  sensor:
    - name: "Smart Meter Total Reading"
      state_topic: "homeassistant/sensor/smartmeter/state"
      device_class: energy
      unit_of_measurement: kWh
      state_class: total_increasing
      value_template: "{{ value_json['+A'] }}"
```

(This publishes raw state, not HA MQTT auto-discovery, to avoid duplicating the
Shelly EM sensors.)

## Meter brand

Default framing = Landis+Gyr E450 / Iskraemeco AM550. For a Siemens meter, set
`-D METER_BRAND_SIEMENS=1` in [platformio.ini](platformio.ini) `build_flags`.

## Notes

- No test suite (embedded firmware; verified on real hardware).
- ESP32 is a secondary reference target (AES via mbedtls); only ESP8266 is built
  today.

## Credits & references

This project builds on prior work — thanks to:

- **[aldadic/esp-smartmeter-reader](https://github.com/aldadic/esp-smartmeter-reader)**
  — the Wiener Netze frame read + CRC + AES-128-CTR decrypt + register-parse
  logic is ported from here.
- **[iobroker-community-adapters/ioBroker.shelly](https://github.com/iobroker-community-adapters/ioBroker.shelly)**
  — reference for the Gen1 Shelly EM (`SHEM`) CoIoT block/sensor IDs
  (`lib/devices/gen1/shellyem.js`).
- **[home-assistant-libs/aioshelly](https://github.com/home-assistant-libs/aioshelly)**
  — the library Home Assistant's Shelly integration uses; the emulation was
  validated against it.
- **[Shelly Gen1 API + CoIoT docs](https://shelly-api-docs.shelly.cloud/gen1/)**
  — protocol reference.
