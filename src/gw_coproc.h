#pragma once

// Wi-Fi co-processor (ESP32-P4 + C6 over ESP-Hosted) support switch.
//
// Needs Arduino-ESP32 >= 3.3.12: that's the first core whose hostedInit()
// waits for the SDIO link and that exposes hostedBeginUpdate/.../
// hostedGetSlaveVersion. On 3.3.3 the link never came up without starting
// the Wi-Fi driver, which is what bricked the C6's Wi-Fi on a Waveshare
// P4-Module-DEV-KIT (see CLAUDE.md). Older cores just don't get the feature.

#include <esp_arduino_version.h>
#include <sdkconfig.h>

#if CONFIG_ESP_HOSTED_ENABLED && ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 3, 12)
#include <esp32-hal-hosted.h>
#define GW_COPROC 1
#else
#define GW_COPROC 0
#endif

#if GW_COPROC && defined(GARNET_WEB_OTA)
#define GW_COPROC_OTA 1
#else
#define GW_COPROC_OTA 0
#endif
