#include "ShellyEM.h"
#include <stdarg.h>
#include <math.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>

// Real Gen1 firmware string. aioshelly enforces a minimum fw date for Gen1
// (>= 20201124); this is a genuine recent SHEM build, so it passes.
static const char* SHEM_FW = "20230913-113421/v1.14.0-gcb84623";

// --- CoIoT / CoAP constants --------------------------------------------------
static const IPAddress COIOT_MCAST(224, 0, 1, 187);   // Shelly CoIoT group
static const uint16_t  COIOT_PORT     = 5683;         // CoAP
static const uint16_t  OPT_DEVID      = 3332;         // global device id (string)
static const uint16_t  OPT_VALIDITY   = 3412;         // status validity
static const uint16_t  OPT_SERIAL     = 3420;         // status serial (must change)
static const uint16_t  COIOT_VALIDITY = 2000;         // ~ generous valid window
static const uint8_t   COAP_GET       = 0x01;         // 0.01
static const uint8_t   COAP_CONTENT   = 0x45;         // 2.05
static const uint8_t   COAP_PUSH      = 0x1E;         // 0.30 (Shelly status push)

// SHEM CoIoT sensor IDs (channel 0): relay output + emeter power/energy/...
// (emeter1 would be 4205-4208; we expose a single channel.)
// IDs and the sensor "D" names below are what HA's BLOCK_SENSORS keys on.

// Append a CoAP option with RFC 7252 extended delta/length (13 -> +1 byte,
// 14 -> +2 bytes). `prev` carries the running option number for delta encoding.
static size_t coapAddOption(uint8_t* buf, size_t pos, uint16_t optNum,
                            uint16_t& prev, const uint8_t* val, uint16_t len) {
    uint16_t delta = optNum - prev;
    prev = optNum;

    uint8_t  hdr;
    uint8_t  dExt[2]; int dExtN = 0;
    uint8_t  lExt[2]; int lExtN = 0;
    uint8_t  dNib, lNib;

    if (delta < 13)       { dNib = delta; }
    else if (delta < 269) { dNib = 13; dExt[0] = delta - 13; dExtN = 1; }
    else                  { dNib = 14; uint16_t e = delta - 269; dExt[0] = e >> 8; dExt[1] = e & 0xFF; dExtN = 2; }

    if (len < 13)         { lNib = len; }
    else if (len < 269)   { lNib = 13; lExt[0] = len - 13; lExtN = 1; }
    else                  { lNib = 14; uint16_t e = len - 269; lExt[0] = e >> 8; lExt[1] = e & 0xFF; lExtN = 2; }

    hdr = (dNib << 4) | lNib;
    buf[pos++] = hdr;
    for (int i = 0; i < dExtN; i++) buf[pos++] = dExt[i];
    for (int i = 0; i < lExtN; i++) buf[pos++] = lExt[i];
    memcpy(buf + pos, val, len);
    pos += len;
    return pos;
}

// Parse an incoming CoAP request: extract type, token, message id, and the last
// Uri-Path segment (to tell /cit/d from /cit/s). Returns true for a GET.
static bool coapParseRequest(const uint8_t* buf, int len, uint8_t* type,
                             uint16_t* mid, uint8_t* token, uint8_t* tkl,
                             bool* isStatus) {
    if (len < 4) return false;
    uint8_t ver = buf[0] >> 6;
    if (ver != 1) return false;
    *type = (buf[0] >> 4) & 0x03;
    *tkl  = buf[0] & 0x0F;
    uint8_t code = buf[1];
    *mid  = (buf[2] << 8) | buf[3];
    int pos = 4;
    if (*tkl > 8 || pos + *tkl > len) return false;
    for (int i = 0; i < *tkl; i++) token[i] = buf[pos++];

    uint16_t optNum = 0;
    char seg[8] = {0};
    while (pos < len && buf[pos] != 0xFF) {
        uint8_t b = buf[pos++];
        uint16_t delta = b >> 4, olen = b & 0x0F;
        if (delta == 13)      { delta = 13 + buf[pos++]; }
        else if (delta == 14) { delta = 269 + ((buf[pos] << 8) | buf[pos + 1]); pos += 2; }
        if (olen == 13)       { olen = 13 + buf[pos++]; }
        else if (olen == 14)  { olen = 269 + ((buf[pos] << 8) | buf[pos + 1]); pos += 2; }
        optNum += delta;
        if (optNum == 11 && olen > 0 && olen < (int)sizeof(seg)) {   // Uri-Path
            memcpy(seg, buf + pos, olen);
            seg[olen] = 0;
        }
        pos += olen;
        if (pos > len) return false;
    }
    *isStatus = (seg[0] == 's');   // "s" -> /cit/s, "d" -> /cit/d
    return code == COAP_GET;
}

ShellyEM::ShellyEM(Stream* logger) : _http(80), _log(logger) {}

void ShellyEM::begin(const char* host) {
    // MAC as 12 uppercase hex, no separators — HA parses this out of the mDNS
    // service instance name (and the CoIoT devid) to identify the device.
    String m = WiFi.macAddress();
    m.replace(":", "");
    m.toUpperCase();
    strncpy(_mac, m.c_str(), sizeof(_mac) - 1);
    snprintf(_devid,   sizeof(_devid),   "shellyem-%s", _mac);
    snprintf(_coiotId, sizeof(_coiotId), "SHEM#%s#2",   _mac);

    setupRoutes();
    _http.begin();

    // mDNS is already running (ArduinoOTA called MDNS.begin). HA's zeroconf step
    // ignores TXT and parses the MAC from the *service instance name*, which must
    // carry the full 12-hex MAC and start with "shelly": shellyem-AABBCCDDEEFF.
    MDNS.setInstanceName(_devid);
    MDNS.addService("http", "tcp", 80);
    MDNS.addServiceTxt("http", "tcp", "id", _devid);
    MDNS.addServiceTxt("http", "tcp", "arch", "esp8266");
    MDNS.addServiceTxt("http", "tcp", "fw_id", SHEM_FW);

    coiotBegin();

    _started = true;
    logf("[shelly] EM up: http://%s/ (mDNS %s.local, id=%s, coiot=%s)",
         WiFi.localIP().toString().c_str(), host, _devid, _coiotId);
}

void ShellyEM::update(const ShellyEMData& d) {
    _data = d;
}

void ShellyEM::handle() {
    if (!_started) return;
    _http.handleClient();
    MDNS.update();
    coiotHandle();
}

float ShellyEM::current() const {
    float v = (_data.voltage > 1.0f) ? _data.voltage : 230.0f;
    return fabsf(_data.power) / v;
}

// ---------------------------------------------------------------- HTTP --------

void ShellyEM::setupRoutes() {
    _http.on("/shelly",   [this]() { _http.send(200, "application/json", shellyJson()); });
    _http.on("/settings", [this]() { _http.send(200, "application/json", settingsJson()); });
    _http.on("/status",   [this]() { _http.send(200, "application/json", statusJson()); });
    _http.on("/emeter/0", [this]() { _http.send(200, "application/json", emeterJson()); });
    // CoIoT description/status are ALSO served over HTTP: aioshelly/HA fetch
    // cit/d (and cit/s) over HTTP at init for fw >= 1.10, then use CoAP for push.
    _http.on("/cit/d",    [this]() { _http.send(200, "application/json", coiotDescription()); });
    _http.on("/cit/s",    [this]() { _serial++; _http.send(200, "application/json", coiotStatus()); });
    _http.on("/relay/0",  [this]() {
        // No real relay; report a stable off state so HA's switch doesn't error.
        _http.send(200, "application/json",
                   "{\"ison\":false,\"has_timer\":false,\"source\":\"http\"}");
    });
    _http.on("/reboot",   [this]() {
        _http.send(200, "application/json", "{\"ok\":true}");
        delay(200);
        ESP.restart();
    });
    _http.onNotFound([this]() {
        _http.send(404, "application/json", "{\"error\":\"not found\"}");
    });
}

String ShellyEM::shellyJson() {
    String s = "{";
    s += "\"type\":\"SHEM\",";
    s += "\"mac\":\"" + String(_mac) + "\",";
    s += "\"auth\":false,";
    s += "\"fw\":\"" + String(SHEM_FW) + "\",";
    s += "\"num_outputs\":1,";
    s += "\"num_meters\":0,";
    s += "\"num_emeters\":1";
    s += "}";
    return s;
}

String ShellyEM::settingsJson() {
    String s = "{";
    s += "\"device\":{\"type\":\"SHEM\",\"mac\":\"" + String(_mac) +
         "\",\"hostname\":\"" + String(_devid) +
         "\",\"num_outputs\":1,\"num_meters\":0,\"num_emeters\":1},";
    s += "\"fw\":\"" + String(SHEM_FW) + "\",";
    s += "\"name\":\"SmartMeter2Shelly\",";
    // HA's ShellyBlockCoordinator reads settings["coiot"]["update_period"];
    // omitting this block makes setup fail with a KeyError.
    s += "\"coiot\":{\"enabled\":true,\"update_period\":15},";
    s += "\"login\":{\"enabled\":false,\"unprotected\":false}";
    s += "}";
    return s;
}

String ShellyEM::emeterJson() {
    char buf[224];
    snprintf(buf, sizeof(buf),
        "{\"power\":%.2f,\"pf\":%.2f,\"current\":%.3f,\"voltage\":%.1f,"
        "\"is_valid\":%s,\"total\":%.1f,\"total_returned\":%.1f,\"reactive\":%.2f}",
        _data.power, _data.pf, current(), _data.voltage,
        _data.valid ? "true" : "false",
        _data.energyWh, _data.energyReturnedWh, _data.reactive);
    return String(buf);
}

String ShellyEM::statusJson() {
    String s = "{";
    s += "\"emeters\":[" + emeterJson() + "],";
    s += "\"relays\":[{\"ison\":false,\"has_timer\":false}],";
    s += "\"mac\":\"" + String(_mac) + "\",";
    s += "\"uptime\":" + String(millis() / 1000);
    s += "}";
    return s;
}

// --------------------------------------------------------------- CoIoT --------

void ShellyEM::coiotBegin() {
    // Bind :5683 and join the CoIoT multicast group. Receives both unicast polls
    // (HA's cit/s) and multicast queries; sends from the same port.
    _udp.beginMulticast(WiFi.localIP(), COIOT_MCAST, COIOT_PORT);
}

// /cit/d — block + sensor description. The sensor "D" names double as the HA/
// aioshelly block attribute names (BLOCK_SENSORS keys), so the emeter MUST use
// power / energy / energyReturned / voltage (NOT total / total_returned).
String ShellyEM::coiotDescription() {
    String d = "{\"blk\":[";
    d += "{\"I\":1,\"D\":\"relay_0\"},";
    d += "{\"I\":2,\"D\":\"emeter_0\"}";
    d += "],\"sen\":[";
    d += "{\"I\":1101,\"T\":\"S\",\"D\":\"output\",\"R\":\"0/1\",\"L\":1},";
    d += "{\"I\":4105,\"T\":\"P\",\"D\":\"power\",\"U\":\"W\",\"R\":\"-3500/3500\",\"L\":2},";
    d += "{\"I\":4106,\"T\":\"E\",\"D\":\"energy\",\"U\":\"Wh\",\"R\":\"U32\",\"L\":2},";
    d += "{\"I\":4107,\"T\":\"E\",\"D\":\"energyReturned\",\"U\":\"Wh\",\"R\":\"U32\",\"L\":2},";
    d += "{\"I\":4108,\"T\":\"V\",\"D\":\"voltage\",\"U\":\"V\",\"R\":\"0/265\",\"L\":2}";
    d += "]}";
    return d;
}

// /cit/s — status values: [channel/flags, sensorId, value].
String ShellyEM::coiotStatus() {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"G\":[[0,1101,0],[0,4105,%.2f],[0,4106,%.1f],[0,4107,%.1f],[0,4108,%.1f]]}",
        _data.power, _data.energyWh, _data.energyReturnedWh, _data.voltage);
    return String(buf);
}

void ShellyEM::coiotSend(IPAddress ip, uint16_t port, uint8_t type, uint8_t code,
                         uint16_t mid, const uint8_t* token, uint8_t tkl,
                         bool statusOpts, const String& payload) {
    uint8_t buf[640];
    size_t  pos = 0;
    buf[pos++] = (1 << 6) | ((type & 0x03) << 4) | (tkl & 0x0F);   // Ver=1
    buf[pos++] = code;
    buf[pos++] = mid >> 8;
    buf[pos++] = mid & 0xFF;
    for (uint8_t i = 0; i < tkl; i++) buf[pos++] = token[i];

    // CoIoT options (ascending): devid always; validity + serial for status.
    uint16_t prev = 0;
    pos = coapAddOption(buf, pos, OPT_DEVID, prev,
                        (const uint8_t*)_coiotId, strlen(_coiotId));
    if (statusOpts) {
        uint8_t v[2] = { (uint8_t)(COIOT_VALIDITY >> 8), (uint8_t)(COIOT_VALIDITY & 0xFF) };
        pos = coapAddOption(buf, pos, OPT_VALIDITY, prev, v, 2);
        uint8_t s[2] = { (uint8_t)(_serial >> 8), (uint8_t)(_serial & 0xFF) };
        pos = coapAddOption(buf, pos, OPT_SERIAL, prev, s, 2);
    }

    if (payload.length()) {
        buf[pos++] = 0xFF;
        memcpy(buf + pos, payload.c_str(), payload.length());
        pos += payload.length();
    }

    _udp.beginPacket(ip, port);
    _udp.write(buf, pos);
    _udp.endPacket();
}

void ShellyEM::coiotHandle() {
    // 1) Answer incoming unicast/multicast CoAP GETs for /cit/d and /cit/s.
    int sz = _udp.parsePacket();
    if (sz > 0) {
        uint8_t in[256];
        int n = _udp.read(in, sizeof(in));
        uint8_t type = 0, tkl = 0, token[8];
        uint16_t mid = 0;
        bool isStatus = false;
        if (n > 0 && coapParseRequest(in, n, &type, &mid, token, &tkl, &isStatus)) {
            IPAddress rip = _udp.remoteIP();
            uint16_t  rport = _udp.remotePort();
            uint8_t   resp = (type == 0) ? 2 : 1;   // ACK to CON, else NON
            if (isStatus) {
                _serial++;
                coiotSend(rip, rport, resp, COAP_CONTENT, mid, token, tkl, true, coiotStatus());
            } else {
                coiotSend(rip, rport, resp, COAP_CONTENT, mid, token, tkl, false, coiotDescription());
            }
        }
    }

    // 2) Periodic multicast status push (best-effort; HA mainly uses the poll).
    if (millis() - _lastPush > 12000) {
        _lastPush = millis();
        _serial++;
        coiotSend(COIOT_MCAST, COIOT_PORT, 1 /*NON*/, COAP_PUSH, _pushMid++,
                  nullptr, 0, true, coiotStatus());
    }
}

void ShellyEM::logf(const char* fmt, ...) {
    if (!_log) return;
    char buf[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    _log->println(buf);
}
