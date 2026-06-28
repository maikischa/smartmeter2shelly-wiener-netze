#pragma once
#include <Arduino.h>
#include <ESP8266WiFi.h>

// -----------------------------------------------------------------------------
// NetLog — a write-only Stream that tees debug output to a local UART *and* a
// telnet client over WiFi. Needed because the USB serial port de-enumerates
// once the WiFi radio is active (current spike in weak signal), so the only
// reliable way to watch the log is over the network: `telnet <host> 23`.
//
// Usage (see src/main.cpp):
//   NetLog LOG(&Serial1, 23);  // tee target = local UART (or nullptr), telnet :23
//   Serial1.begin(115200);     // bring up the local UART yourself
//   ... once WiFi is connected:  LOG.beginServer();
//   ... every loop():            LOG.handle();
// Pass &LOG anywhere a Stream*/Print* logger is expected (e.g. SmartMeter).
// `local` may be nullptr to disable the serial tee (telnet-only logging).
// Skipping beginServer() disables the telnet tee. One telnet viewer at a time;
// a new connection replaces a dead one.
// -----------------------------------------------------------------------------
class NetLog : public Stream {
public:
    NetLog(Print* local, uint16_t port = 23);

    void beginServer();   // start the telnet server (call once WiFi is up)
    void handle();        // accept/drop telnet clients (call from loop())
    bool hasClient();     // true while a telnet viewer is connected

    // Print — tee to the local UART and the telnet client (if any).
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* buf, size_t size) override;
    using Print::write;   // keep the inherited write() overloads visible

    // Stream — this is a write-only logger, nothing to read.
    int  available() override { return 0; }
    int  read() override { return -1; }
    int  peek() override { return -1; }
    void flush() override {}

private:
    Print*      _local;          // local UART tee target; nullptr = disabled
    WiFiServer  _server;
    WiFiClient  _client;
    bool        _started = false;
};
