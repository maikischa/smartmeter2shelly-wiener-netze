#pragma once
#include <Arduino.h>

// -----------------------------------------------------------------------------
// SmartMeter — reads Wiener Netze smart meter frames over the optical IR head,
// validates the CRC, decrypts the DLMS payload (AES-128-CTR) and parses the
// energy / power registers.
//
// Logic ported from aldadic/esp-smartmeter-reader.
// -----------------------------------------------------------------------------

// --- Per-brand HDLC/DLMS framing constants -----------------------------------
#if defined(METER_BRAND_SIEMENS)
  #define SM_MESSAGE_LENGTH 125
  #define SM_HEADER_LENGTH  16
#else  // Landis+Gyr E450 / Iskraemeco AM550 (default)
  #define SM_MESSAGE_LENGTH 105
  #define SM_HEADER_LENGTH  14
#endif
#define SM_PAYLOAD_LENGTH (SM_MESSAGE_LENGTH - SM_HEADER_LENGTH - 17)

// Decoded meter snapshot. Energy registers are in Wh / varh (raw value);
// divide by 1000 for kWh / kvarh. Power registers are already in W / var.
struct MeterReading {
    bool valid = false;

    // Timestamp reported by the meter
    uint16_t year = 0;
    uint8_t  month = 0, day = 0;
    uint8_t  hour = 0, minute = 0, second = 0;

    uint32_t activeImportWh    = 0;  // +A
    uint32_t activeExportWh    = 0;  // -A
    uint32_t reactiveImport    = 0;  // +R (varh)
    uint32_t reactiveExport    = 0;  // -R (varh)

    uint32_t activeImportW     = 0;  // +P
    uint32_t activeExportW     = 0;  // -P
    uint32_t reactiveImportVar = 0;  // +Q
    uint32_t reactiveExportVar = 0;  // -Q
};

class SmartMeter {
public:
    // `meter`  : the UART the IR head is on (already begun + pin-mapped by caller)
    // `key`    : 16-byte AES-128 key
    // `logger` : optional Stream for debug output (nullptr = silent)
    SmartMeter(Stream& meter, const uint8_t key[16], Stream* logger = nullptr);

    void begin();

    // Poll the meter UART. Returns true and fills `out` once a complete, valid,
    // decrypted frame has been parsed. Call frequently from loop().
    bool poll(MeterReading& out);

private:
    Stream&  _meter;
    Stream*  _log;
    uint8_t  _key[16];

    uint8_t  _buf[SM_MESSAGE_LENGTH];
    size_t   _len = 0;
    bool     _inFrame = false;

    bool     checkCrc();
    bool     decrypt(uint8_t* plaintextOut);
    void     parse(const uint8_t* payload, MeterReading& out);
    uint32_t bytesToInt(const uint8_t* payload, int offsetFromEnd, int len);
    void     logln(const char* msg);
    void     logf(const char* fmt, ...);
};
