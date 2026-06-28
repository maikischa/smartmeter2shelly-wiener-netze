#pragma once
#include <Arduino.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>

// -----------------------------------------------------------------------------
// ShellyEM — emulates a Gen1 Shelly EM (device type "SHEM", 1 emeter channel) so
// Home Assistant and any Shelly-API consumer see the smart meter as a native,
// bidirectional energy meter.
//
// A Gen1 Shelly speaks two protocols, both implemented here:
//   * HTTP (ESP8266WebServer :80) — identity + config, used for zeroconf/manual
//     discovery and HA's config flow.
//   * CoIoT/CoAP (WiFiUDP :5683)  — the live block sensors HA actually reads.
//     HA polls /cit/d (block description) + /cit/s (status) over unicast and may
//     also receive multicast (224.0.1.187) push frames. Every status message
//     MUST carry the CoIoT device-id option or aioshelly/HA reject it.
//
// Reusable + config-independent: construct with a logger, call begin(host) once
// WiFi + mDNS are up (the caller owns mDNS — ArduinoOTA starts it here), feed the
// latest values via update(), and call handle() every loop().
// -----------------------------------------------------------------------------

// Latest electrical snapshot to expose as one Shelly EM channel.
// Energy is in Wh — HA's Gen1 emeter energy sensor is native Wh (no scaling).
struct ShellyEMData {
    bool   valid            = false;
    float  power            = 0.0f;   // W, signed (+ = import / consumption)
    float  reactive         = 0.0f;   // var, signed
    float  voltage          = 230.0f; // V (synthesized; WN meter sends none)
    float  pf               = 1.0f;
    double energyWh         = 0.0;    // total imported (+A), Wh
    double energyReturnedWh = 0.0;    // total exported (-A), Wh
};

class ShellyEM {
public:
    explicit ShellyEM(Stream* logger = nullptr);

    void begin(const char* host);    // host = mDNS hostname already begun by caller
    void update(const ShellyEMData& d);
    void handle();                   // call every loop()

    const char* deviceId() const { return _devid; }   // "shellyem-AABBCCDDEEFF"
    const char* mac() const { return _mac; }           // "AABBCCDDEEFF"

private:
    ESP8266WebServer _http;
    WiFiUDP          _udp;             // CoIoT/CoAP on :5683
    ShellyEMData     _data;
    Stream*          _log;
    char             _mac[13]      = {0};
    char             _devid[24]    = {0};  // mDNS instance "shellyem-<MAC>"
    char             _coiotId[24]  = {0};  // CoIoT devid "SHEM#<MAC>#2"
    uint16_t         _serial       = 0;    // CoIoT status serial (must change)
    uint16_t         _pushMid      = 0;    // CoAP message id for pushes
    unsigned long    _lastPush     = 0;
    bool             _started      = false;

    float  current() const;          // |power| / voltage

    // HTTP
    void   setupRoutes();
    String shellyJson();
    String settingsJson();
    String statusJson();
    String emeterJson();

    // CoIoT
    void   coiotBegin();
    void   coiotHandle();
    String coiotDescription();       // {blk,sen} for /cit/d
    String coiotStatus();            // {G:[[...]]} for /cit/s
    void   coiotSend(IPAddress ip, uint16_t port, uint8_t type, uint8_t code,
                     uint16_t mid, const uint8_t* token, uint8_t tkl,
                     bool statusOpts, const String& payload);

    void   logf(const char* fmt, ...);
};
