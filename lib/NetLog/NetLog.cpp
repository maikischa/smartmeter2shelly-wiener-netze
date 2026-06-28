#include "NetLog.h"

NetLog::NetLog(Print* local, uint16_t port)
    : _local(local), _server(port) {}

void NetLog::beginServer() {
    if (_started) return;
    _server.begin();
    _server.setNoDelay(true);   // don't coalesce small log writes
    _started = true;
}

void NetLog::handle() {
    if (!_started) return;
    if (!_server.hasClient()) return;

    if (hasClient()) {
        // Already have a viewer — refuse the extra connection.
        _server.accept().stop();
        return;
    }
    if (_client) _client.stop();          // clean up a previous dead client
    _client = _server.accept();           // accept the pending connection
    _client.setNoDelay(true);
    _client.println(F("== smartmeter2shelly telnet log =="));
}

bool NetLog::hasClient() {
    return _client && _client.connected();
}

size_t NetLog::write(uint8_t c) {
    if (hasClient()) _client.write(c);
    return _local ? _local->write(c) : 1;
}

size_t NetLog::write(const uint8_t* buf, size_t size) {
    if (hasClient()) _client.write(buf, size);
    return _local ? _local->write(buf, size) : size;
}
