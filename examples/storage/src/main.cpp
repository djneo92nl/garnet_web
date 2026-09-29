// Storage firmware - flash this on a spare board before shelving it.
//
// Next time it's powered: join the open AP "<chip>-XXXX" (e.g.
// "ESP32-C3-1E08"), sign in, and you see what the board is (chip, flash,
// PSRAM, Chip ID, the label/notes you left), can put it on Wi-Fi, and can
// install the real firmware under System > Firmware > Update.
//
// Files: the flash data partition is browsable under Files (LittleFS).
//
// Deliberately nothing else - no BT, no SD, no pins touched - so it runs
// unchanged on any Wi-Fi ESP32 and can't fight whatever hardware the
// board ends up soldered to.

#include <Arduino.h>
#include <LittleFS.h>
#include <garnet_web.h>

namespace {

// What you'd otherwise write on masking tape.
const GsField kBoardFields[] = {
    gsWithPlaceholder(gsText("label", "Label", "", 32), "e.g. spare #3"),
    gsWithPlaceholder(gsText("notes", "Notes", "", 120), "wiring, faults, where it came from"),
};
const GsGroup kBoard = gsGroup("board", "Board", "lock", kBoardFields);

// One line on serial whenever the network state changes, so a board
// plugged into USB tells you where to find it without any log level.
void printStatus(GwNetMode mode) {
  const char *what = mode == GwNetMode::WiFi       ? "Wi-Fi"
                     : mode == GwNetMode::Ethernet ? "Ethernet"
                     : mode == GwNetMode::Portal   ? "setup AP"
                                                   : "offline";
  Serial.printf("[storage] %s %s  id %s  http://%s/  (%s.local)\n", ESP.getChipModel(), what,
                gwChipId().c_str(), gwNetIP().toString().c_str(), gwHostname().c_str());
}

} // namespace

void setup() {
  Serial.begin(115200);
  gsRegister(kBoard);
  // The table's data partition (1.4 MB on default.csv) as LittleFS: room for
  // a datasheet, a pinout photo or the project's config next to the board.
  // Formatted on first boot.
  if (LittleFS.begin(true)) gwAddFs("flash", "Flash", LittleFS);

  GwConfig cfg;
  cfg.name = ESP.getChipModel(); // "ESP32-C3" etc: AP name + hostname prefix
  cfg.defaultPassword = "changeme";
  cfg.appVersion = "storage 1.0";
  gwBegin(cfg);
}

void loop() {
  gwLoop();
  static GwNetMode last = GwNetMode::Offline;
  static bool printed = false;
  if (!printed || gwNetMode() != last) {
    last = gwNetMode();
    printed = true;
    printStatus(last);
  }
  delay(5); // idle politely - it may sit on a USB charger for days
}
