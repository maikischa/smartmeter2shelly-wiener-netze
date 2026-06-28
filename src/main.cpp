#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include <SmartMeter.h>
#include <NetLog.h>
#include "config.h"
#include "secrets.h"
#if ENABLE_SHELLY_EM
  #include <ShellyEM.h>
#endif
#if ENABLE_MQTT
  #include <MeterMqtt.h>
#endif

// -----------------------------------------------------------------------------
// SmartMeter2Shelly — read + decrypt + parse a Wiener Netze meter, then expose
// it as: a Shelly EM (HTTP + CoIoT, for Home Assistant) and/or MQTT, with a
// telnet + serial debug log. Each output is toggled in include/config.h.
//
// The USB serial port de-enumerates once the WiFi radio is active, so the log is
// teed to a telnet server (NetLog): `telnet smartmeter2shelly.local` (port 23),
// and still to the local UART (Serial1) for the USB-TTL/bridge case.
//
// D1 mini wiring (see README.md):
//   IR head RX -> GPIO13 (D7), TX -> GPIO15 (D8). Meter is on UART0 after
//   Serial.swap(); local debug log goes out UART1 (Serial1, GPIO2). Bridge
//   GPIO2 (D4) -> GPIO1 (TX) to read that over USB at 115200 baud.
//
// OTA: first flash over USB, then set upload_protocol=espota in platformio.ini.
//      ArduinoOTA.handle() is called every loop() so updates are always possible.
// -----------------------------------------------------------------------------

#define DBG_UART Serial1   // local debug UART (UART1, GPIO2, TX-only)

// LOG tees to the local UART (only if ENABLE_SERIAL_LOG) and to telnet (only if
// ENABLE_TELNET_LOG, which gates beginServer()/handle() below).
NetLog LOG(ENABLE_SERIAL_LOG ? &DBG_UART : nullptr, 23);

// Meter on UART0 (swapped), logging to LOG.
SmartMeter meter(Serial, KEY, &LOG);

#if ENABLE_SHELLY_EM
ShellyEM shelly(&LOG);     // Shelly EM emulation (HTTP + CoIoT); fed per frame
#endif
#if ENABLE_MQTT
MeterMqtt mqtt(MQTT_SERVER, MQTT_PORT, MQTT_USER, MQTT_PASS,
               MQTT_TOPIC, OTA_HOSTNAME, &LOG);
#endif

static bool otaStarted = false;
static unsigned long lastWaitLog = 0;
#if ENABLE_SHELLY_EM
static bool shellyStarted = false;
#endif
#if ENABLE_MQTT
static unsigned long lastMqttPub = 0;
#endif

static void startOTA() {
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    if (strlen(OTA_PASSWORD) > 0) {
        ArduinoOTA.setPassword(OTA_PASSWORD);
    }
    ArduinoOTA.onStart([]() { LOG.println(F("[OTA] update starting")); });
    ArduinoOTA.onEnd([]()   { LOG.println(F("\n[OTA] done -> rebooting")); });
    ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
        LOG.printf("[OTA] %u%%\r", (t == 0) ? 0 : (p * 100) / t);
    });
    ArduinoOTA.onError([](ota_error_t e) { LOG.printf("[OTA] error[%u]\n", e); });
    ArduinoOTA.begin();
    otaStarted = true;
    LOG.printf("[OTA] ready: host='%s.local' ip=%s\n",
               OTA_HOSTNAME, WiFi.localIP().toString().c_str());

#if ENABLE_TELNET_LOG
    // Telnet log server (needs WiFi up). Watch with: telnet <host>.local
    LOG.beginServer();
    LOG.printf("[net] telnet log ready: 'telnet %s.local' (port 23)\n",
               OTA_HOSTNAME);
#endif
    // NOTE: ShellyEM is started later, on the first valid meter frame (see loop),
    // so it never serves a zero/empty energy reading that HA's total_increasing
    // sensor would mistake for a meter reset (false energy-dashboard spike).
}

void setup() {
#if ENABLE_SERIAL_LOG
    DBG_UART.begin(115200);   // bring up the local UART; LOG tees here + telnet
#endif
    LOG.println();
    LOG.println(F("== SmartMeter2Shelly =="));

    // WiFi station
    WiFi.persistent(false);          // don't rewrite stored creds to flash each boot
    WiFi.mode(WIFI_STA);
    WiFi.hostname(OTA_HOSTNAME);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    LOG.printf("[WiFi] connecting to '%s'", WIFI_SSID);
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {  // up to ~10 s
        delay(250);
        LOG.print('.');
    }
    LOG.println();
    if (WiFi.status() == WL_CONNECTED) {
        LOG.printf("[WiFi] connected ip=%s rssi=%d dBm\n",
                   WiFi.localIP().toString().c_str(), WiFi.RSSI());
        startOTA();
    } else {
        LOG.println(F("[WiFi] not up yet - retrying in background"));
    }

#if ENABLE_MQTT
    mqtt.begin();
#endif

    // IR head on UART0 @ 9600 8N1, then remap UART0 to GPIO15/GPIO13 (off USB pins)
    Serial.begin(9600);
    Serial.swap();
    meter.begin();
}

void loop() {
    // Bring OTA up once WiFi is connected (covers a late/background connect).
    if (WiFi.status() == WL_CONNECTED) {
        if (!otaStarted) startOTA();
        ArduinoOTA.handle();
#if ENABLE_TELNET_LOG
        LOG.handle();              // accept/drop telnet log viewers
#endif
#if ENABLE_SHELLY_EM
        shelly.handle();           // Shelly EM HTTP + CoIoT
#endif
#if ENABLE_MQTT
        mqtt.loop();               // keep the MQTT connection alive
#endif
    } else if (millis() - lastWaitLog > 10000) {
        lastWaitLog = millis();
        LOG.println(F("[WiFi] waiting for connection..."));
    }

    // Meter read
    MeterReading r;
    if (meter.poll(r)) {
        LOG.println(F("---- frame ----"));
        LOG.printf("time   : %04u-%02u-%02u %02u:%02u:%02u\n",
                   r.year, r.month, r.day, r.hour, r.minute, r.second);
        LOG.printf("+A     : %lu Wh   (%.3f kWh)\n",
                   (unsigned long)r.activeImportWh, r.activeImportWh / 1000.0);
        LOG.printf("-A     : %lu Wh   (%.3f kWh)\n",
                   (unsigned long)r.activeExportWh, r.activeExportWh / 1000.0);
        LOG.printf("+R     : %lu varh (%.3f kvarh)\n",
                   (unsigned long)r.reactiveImport, r.reactiveImport / 1000.0);
        LOG.printf("-R     : %lu varh (%.3f kvarh)\n",
                   (unsigned long)r.reactiveExport, r.reactiveExport / 1000.0);
        LOG.printf("+P     : %lu W    -P: %lu W\n",
                   (unsigned long)r.activeImportW, (unsigned long)r.activeExportW);
        LOG.printf("+Q     : %lu var  -Q: %lu var\n",
                   (unsigned long)r.reactiveImportVar, (unsigned long)r.reactiveExportVar);

#if ENABLE_SHELLY_EM
        // Map the meter frame onto one Shelly EM channel. Power is signed with
        // + = import; energy is fed in Wh (HA's emeter energy sensor is Wh-native).
        ShellyEMData s;
        s.valid            = true;
        s.power            = (float)((int32_t)r.activeImportW - (int32_t)r.activeExportW);
        s.reactive         = (float)((int32_t)r.reactiveImportVar - (int32_t)r.reactiveExportVar);
        s.voltage          = 230.0f;   // WN meter sends no voltage; nominal
        s.pf               = 1.0f;
        s.energyWh         = (double)r.activeImportWh;
        s.energyReturnedWh = (double)r.activeExportWh;
        shelly.update(s);

        // Bring the Shelly EM online on the first valid frame (mDNS is already up
        // via ArduinoOTA). Deferring until here guarantees it never advertises a
        // zero energy reading right after boot.
        if (!shellyStarted && WiFi.status() == WL_CONNECTED) {
            shelly.begin(OTA_HOSTNAME);
            shellyStarted = true;
        }
#endif

#if ENABLE_MQTT
        if (millis() - lastMqttPub >= MQTT_PUBLISH_INTERVAL_MS) {
            lastMqttPub = millis();
            mqtt.publish(r);
        }
#endif
    }
}
