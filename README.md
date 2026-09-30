# garnet_web

A Garnet-styled (Palm OS 5) two-column web settings UI for ESP32 that
replaces WiFiManager. It renders [garnet_settings](../garnet_settings)
groups in the browser and adds Wi-Fi (with a captive-portal setup AP),
Ethernet, Bluetooth, SD and System groups by build flag.

```cpp
#include <garnet_web.h>

static const GsField kFields[] = {
  gsToggle("on", "Relay on", false, onRelay),
  gsNumber("pulse", "Pulse (ms)", 500, 50, 5000, 50),
  gsButton("pulse", "Pulse now", pulse, "Close the relay?"),
};
static const GsGroup kRelay = gsGroup("relay", "Relay", "power", kFields);

void setup() {
  gsRegister(kRelay);
  GwConfig cfg;
  cfg.name = "Relay Box";
  cfg.defaultPassword = "changeme";
  gwBegin(cfg);
}
void loop() { gwLoop(); }
```

```ini
platform = https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip
build_flags = -D GARNET_WEB_WIFI -D GARNET_WEB_BT
lib_deps =
  https://github.com/<you>/garnet_settings.git   ; or symlink://path/to/garnet_settings
  https://github.com/<you>/garnet_web.git
```

Examples: `examples/esp32s3_camera` (camera live view + SD), `examples/wt32_eth01` (Ethernet), `examples/cyd` (SD).
UI development without hardware: `python3 tools/mock_server.py`.
The storage / bench firmware built on this library (every Wi-Fi ESP32, OTA,
file manager, I2C/GPIO/serial tools) lives in its own repo,
[esp32-storage](../esp32-storage).
See CLAUDE.md for the design rules.
