#pragma once
// -----------------------------------------------------------------------------
// config.h — feature toggles. Flip 1 / 0 and re-flash (OTA is fine).
//
// This file is ONLY on/off switches. Credentials and network/broker settings
// live in secrets.h. Disabled features are compiled out entirely (no flash/RAM
// cost), so turning something off also frees space.
// -----------------------------------------------------------------------------

#define ENABLE_SERIAL_LOG  1   // local debug log on UART1 (Serial1 / GPIO2)
#define ENABLE_TELNET_LOG  1   // debug log over WiFi (telnet :23)
#define ENABLE_SHELLY_EM   1   // emulate a Shelly EM (HTTP + CoIoT) for Home Assistant
#define ENABLE_MQTT        1   // publish meter readings to MQTT (broker in secrets.h)

// How often to publish to MQTT, in ms (the meter pushes ~1 frame/second).
#define MQTT_PUBLISH_INTERVAL_MS 3000
