// garnet_web example: ESP32-S3 camera board - WiFi + BLE + SD card, with an
// OV5640 / OV3660 live view and camera settings (camera_group.cpp).
//
// First boot: no WiFi saved -> join the open AP "S3 Demo-XXXX" from a
// phone, the captive portal opens the settings page, sign in with the
// password below, pick your network under Wi-Fi.

#include <Arduino.h>
#include <LittleFS.h>
#include <garnet_web.h>

#include "camera_group.h"

namespace {

// ---- An app group exercising every field type ------------------------------

constexpr const char *kDemo = "demo";
const char *const kModes[] = {"Eco", "Normal", "Boost"};
uint32_t presses = 0;

void onLevel(const GsField &) {
  Serial.printf("level -> %d\n", (int)gsGetInt(kDemo, "level"));
}
void onDemoSaved(const GsGroup &) { Serial.println("demo group saved"); }
void sayHello() { Serial.printf("hello #%u from the web UI\n", (unsigned)++presses); }
String pressesStr() { return String(presses); }
String cpuTempStr() { return String(temperatureRead(), 1) + " \xC2\xB0" "C"; }

const GsField kDemoFields[] = {
    gsToggle("enabled", "Enabled", true),
    gsWithHelp(gsNumber("level", "Level", 50, 0, 100, 5, onLevel), "0-100, steps of 5"),
    gsSelect("mode", "Mode", kModes, 3, 1),
    gsText("label", "Label", "Workbench", 24),
    gsWithShowIf(gsPassword("apikey", "API key"), "enabled"),
    gsButton("hello", "Say Hello", sayHello),
    gsInfo("presses", "Hello count", pressesStr),
    gsInfo("temp", "Chip temperature", cpuTempStr),
};
const GsGroup kDemoGroup =
    gsWithOnSave(gsGroup(kDemo, "Demo", "sliders", kDemoFields), onDemoSaved);

} // namespace

void setup() {
  Serial.begin(115200);
  bool haveCamera = cameraBegin(); // registers "Camera" (first in the sidebar section)
  gsRegister(kDemoGroup);
  if (LittleFS.begin(true)) gwAddFs("flash", "Flash", LittleFS); // data partition of default_16MB.csv

  GwConfig cfg;
  cfg.name = "S3 Demo";
  cfg.defaultPassword = "changeme"; // change in System > Change Password
  cfg.appVersion = "0.1.0";
  gwBegin(cfg);
  if (haveCamera) cameraStartStream();
}

void loop() {
  gwLoop();
  delay(2);
}
