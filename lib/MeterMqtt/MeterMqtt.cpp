#include "MeterMqtt.h"
#include <stdarg.h>

MeterMqtt::MeterMqtt(const char* server, int port,
                     const char* user, const char* pass,
                     const char* stateTopic, const char* clientId,
                     Stream* logger)
    : _mqtt(_wifi), _server(server), _port(port), _user(user), _pass(pass),
      _stateTopic(stateTopic), _clientId(clientId), _log(logger) {
    // Availability topic = sibling of the state topic: ".../<x>/availability".
    _availTopic = String(stateTopic);
    int slash = _availTopic.lastIndexOf('/');
    if (slash > 0) _availTopic = _availTopic.substring(0, slash);
    _availTopic += "/availability";
}

void MeterMqtt::begin() {
    _mqtt.setServer(_server, _port);
    _mqtt.setBufferSize(512);   // default 256 is too small for the JSON payload
    logf("[mqtt] broker=%s:%d topic=%s avail=%s",
         _server, _port, _stateTopic, _availTopic.c_str());
}

bool MeterMqtt::reconnect() {
    if (_mqtt.connected()) return true;
    if (millis() - _lastReconnect < 5000) return false;   // throttle attempts
    _lastReconnect = millis();

    logf("[mqtt] connecting to %s:%d ...", _server, _port);
    bool ok;
    const char* user = (strlen(_user) > 0) ? _user : nullptr;
    const char* pass = (strlen(_pass) > 0) ? _pass : nullptr;
    // LWT: broker marks us "offline" on the availability topic if we drop.
    ok = _mqtt.connect(_clientId.c_str(), user, pass,
                       _availTopic.c_str(), 0, true, "offline");
    if (ok) {
        _mqtt.publish(_availTopic.c_str(), "online", true);
        logf("[mqtt] connected as '%s'", _clientId.c_str());
    } else {
        logf("[mqtt] connect failed, rc=%d (retry in 5s)", _mqtt.state());
    }
    return ok;
}

void MeterMqtt::loop() {
    if (!_mqtt.connected()) reconnect();
    _mqtt.loop();
}

void MeterMqtt::publish(const MeterReading& r) {
    if (!_mqtt.connected()) return;

    char ts[32];
    snprintf(ts, sizeof(ts), "%04u-%02u-%02uT%02u:%02u:%02u",
             r.year, r.month, r.day, r.hour, r.minute, r.second);

    // OBIS-style keys to match the HA MQTT sensor value_templates:
    //   energy +A/-A and reactive +R/-R are published in kWh / kvarh (the
    //   meter's raw Wh/varh divided by 1000 — the HA energy sensors are kWh and
    //   do NOT divide). Power +P/-P and reactive power +Q/-Q stay in W / var.
    char buf[420];
    snprintf(buf, sizeof(buf),
        "{\"time\":\"%s\","
        "\"+A\":%.3f,\"-A\":%.3f,\"+R\":%.3f,\"-R\":%.3f,"
        "\"+P\":%lu,\"-P\":%lu,\"+Q\":%lu,\"-Q\":%lu}",
        ts,
        r.activeImportWh / 1000.0, r.activeExportWh / 1000.0,
        r.reactiveImport / 1000.0, r.reactiveExport / 1000.0,
        (unsigned long)r.activeImportW,     (unsigned long)r.activeExportW,
        (unsigned long)r.reactiveImportVar, (unsigned long)r.reactiveExportVar);

    if (!_mqtt.publish(_stateTopic, buf, true)) {   // retained
        logf("[mqtt] publish failed (payload %u bytes)", (unsigned)strlen(buf));
    }
}

void MeterMqtt::logf(const char* fmt, ...) {
    if (!_log) return;
    char buf[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    _log->println(buf);
}
