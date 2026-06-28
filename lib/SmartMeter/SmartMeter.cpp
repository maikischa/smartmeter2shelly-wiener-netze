#include "SmartMeter.h"
#include <stdarg.h>
#include <FastCRC.h>

#if defined(ESP8266)
  #include <Crypto.h>
  #include <AES.h>
  #include <CTR.h>
#elif defined(ESP32)
  #include "mbedtls/aes.h"
#endif

SmartMeter::SmartMeter(Stream& meter, const uint8_t key[16], Stream* logger)
    : _meter(meter), _log(logger) {
    memcpy(_key, key, 16);
}

void SmartMeter::begin() {
    logf("SmartMeter ready (brand frame=%d header=%d payload=%d)",
         SM_MESSAGE_LENGTH, SM_HEADER_LENGTH, SM_PAYLOAD_LENGTH);
}

bool SmartMeter::poll(MeterReading& out) {
    while (_meter.available()) {
        uint8_t b = (uint8_t)_meter.read();

        if (!_inFrame) {
            // Look for the HDLC start flag.
            if (b == 0x7E) {
                _buf[0] = b;
                _len = 1;
                _inFrame = true;   // tentative; confirmed by 0xA0 next
            }
            continue;
        }

        if (_len == 1) {
            // Second byte must be 0xA0; otherwise resync.
            if (b == 0xA0) {
                _buf[_len++] = b;
            } else if (b == 0x7E) {
                _len = 1;          // another flag — restart on it
            } else {
                _inFrame = false;
                _len = 0;
            }
            continue;
        }

        _buf[_len++] = b;

        if (_len >= SM_MESSAGE_LENGTH) {
            _inFrame = false;
            _len = 0;

            if (!checkCrc()) {
                return false;
            }
            uint8_t plain[SM_PAYLOAD_LENGTH];
            if (!decrypt(plain)) {
                return false;
            }
            parse(plain, out);
            out.valid = true;
            return true;
        }
    }
    return false;
}

bool SmartMeter::checkCrc() {
    FastCRC16 crc16;
    uint16_t crc = crc16.x25(_buf + 1, SM_MESSAGE_LENGTH - 4);
    uint16_t expected = (uint16_t)_buf[SM_MESSAGE_LENGTH - 2] * 256 +
                        _buf[SM_MESSAGE_LENGTH - 3];
    if (crc != expected) {
        logf("CRC mismatch: got %04X expected %04X", crc, expected);
        return false;
    }
    return true;
}

bool SmartMeter::decrypt(uint8_t* plaintextOut) {
    // Build the 16-byte CTR initial counter block (IV):
    //   [0..7]  system title, [8..11] frame counter, [12..14] 0, [15] 0x02
    uint8_t iv[16];
    memset(iv, 0, sizeof(iv));
    memcpy(iv, _buf + SM_HEADER_LENGTH, 8);
    memcpy(iv + 8, _buf + SM_HEADER_LENGTH + 10, 4);
    iv[15] = 0x02;

    const uint8_t* ciphertext = _buf + SM_HEADER_LENGTH + 14;

#if defined(ESP8266)
    CTR<AES128> ctr;
    if (!ctr.setKey(_key, 16)) {
        logln("AES setKey failed");
        return false;
    }
    ctr.setIV(iv, 16);
    ctr.decrypt(plaintextOut, ciphertext, SM_PAYLOAD_LENGTH);
    return true;
#elif defined(ESP32)
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, _key, 128);
    size_t nc_off = 0;
    uint8_t stream_block[16] = {0};
    uint8_t nonce[16];
    memcpy(nonce, iv, 16);
    int rc = mbedtls_aes_crypt_ctr(&aes, SM_PAYLOAD_LENGTH, &nc_off, nonce,
                                   stream_block, ciphertext, plaintextOut);
    mbedtls_aes_free(&aes);
    if (rc != 0) {
        logf("mbedtls AES-CTR failed: %d", rc);
        return false;
    }
    return true;
#else
    #error "Unsupported platform: no AES-CTR implementation"
#endif
}

uint32_t SmartMeter::bytesToInt(const uint8_t* p, int offsetFromEnd, int len) {
    int start = (int)SM_PAYLOAD_LENGTH + offsetFromEnd;  // offsetFromEnd is negative
    uint32_t v = 0;
    for (int i = 0; i < len; i++) {
        v = (v << 8) | p[start + i];
    }
    return v;
}

void SmartMeter::parse(const uint8_t* p, MeterReading& out) {
    out.year   = (uint16_t)bytesToInt(p, -52, 2);
    out.month  = (uint8_t)bytesToInt(p, -50, 1);
    out.day    = (uint8_t)bytesToInt(p, -49, 1);
    out.hour   = (uint8_t)bytesToInt(p, -47, 1);
    out.minute = (uint8_t)bytesToInt(p, -46, 1);
    out.second = (uint8_t)bytesToInt(p, -45, 1);

    out.activeImportWh    = bytesToInt(p, -39, 4);
    out.activeExportWh    = bytesToInt(p, -34, 4);
    out.reactiveImport    = bytesToInt(p, -29, 4);
    out.reactiveExport    = bytesToInt(p, -24, 4);
    out.activeImportW     = bytesToInt(p, -19, 4);
    out.activeExportW     = bytesToInt(p, -14, 4);
    out.reactiveImportVar = bytesToInt(p, -9, 4);
    out.reactiveExportVar = bytesToInt(p, -4, 4);
}

void SmartMeter::logln(const char* msg) {
    if (_log) _log->println(msg);
}

void SmartMeter::logf(const char* fmt, ...) {
    if (!_log) return;
    char buf[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    _log->println(buf);
}
