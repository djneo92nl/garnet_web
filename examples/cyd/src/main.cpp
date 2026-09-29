// garnet_web example: CYD (ESP32-2432S028R) with SD card info and a
// Display group - the same table a garnet_ui settings screen can render
// on-device once the garnet_settings menu adapter exists.

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <garnet_web.h>

namespace {

// CYD SD slot is on VSPI.
constexpr int kSdCs = 5, kSdSck = 18, kSdMiso = 19, kSdMosi = 23;
constexpr int kBacklightPin = 21;
constexpr const char *kDisplay = "display";

SPIClass sdSpi(VSPI);

void applyBrightness(const GsField &) {
  analogWrite(kBacklightPin, gsGetInt(kDisplay, "bright"));
}

const char *const kTimeouts[] = {"Never", "30 s", "1 min", "2 min", "5 min"};

const GsField kDisplayFields[] = {
    gsNumber("bright", "Brightness", 255, 10, 255, 5, applyBrightness),
    gsToggle("dark", "Dark mode", false),
    gsSelect("timeout", "Screen off after", kTimeouts, 5, 3),
};
const GsGroup kDisplayGroup = gsGroup(kDisplay, "Display", "display", kDisplayFields);

} // namespace

void setup() {
  Serial.begin(115200);
  gsRegister(kDisplayGroup);
  pinMode(kBacklightPin, OUTPUT);
  applyBrightness(kDisplayFields[0]);

  sdSpi.begin(kSdSck, kSdMiso, kSdMosi, kSdCs);
  if (SD.begin(kSdCs, sdSpi)) gwSetSd(SD);

  GwConfig cfg;
  cfg.name = "CYD";
  cfg.defaultPassword = "changeme";
  cfg.appVersion = "0.1.0";
  gwBegin(cfg);
}

void loop() { gwLoop(); }
