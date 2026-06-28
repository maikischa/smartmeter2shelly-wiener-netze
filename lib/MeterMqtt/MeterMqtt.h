#pragma once
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include "SmartMeter.h"   // MeterReading

// -----------------------------------------------------------------------------
// MeterMqtt — publishes decoded meter readings to an MQTT broker as a single
// JSON state message. Non-blocking: keeps the connection alive with throttled
// reconnects and a Last-Will "offline" on an availability topic.
//
// Usage: construct with broker/topic (from secrets.h), begin() once, loop()
// every loop(), and publish(reading) when you have a fresh frame.
// -----------------------------------------------------------------------------
class MeterMqtt {
public:
    MeterMqtt(const char* server, int port,
              const char* user, const char* pass,
              const char* stateTopic, const char* clientId,
              Stream* logger = nullptr);

    void begin();
    void loop();                          // maintain connection; call every loop()
    void publish(const MeterReading& r);  // publish the reading as retained JSON
    bool connected() { return _mqtt.connected(); }

private:
    WiFiClient    _wifi;
    PubSubClient  _mqtt;
    const char*   _server;
    int           _port;
    const char*   _user;
    const char*   _pass;
    const char*   _stateTopic;
    String        _clientId;
    String        _availTopic;
    Stream*       _log;
    unsigned long _lastReconnect = 0;

    bool reconnect();
    void logf(const char* fmt, ...);
};
