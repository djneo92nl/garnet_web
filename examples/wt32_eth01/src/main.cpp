// garnet_web example: WT32-ETH01 relay box - Ethernet + WiFi fallback +
// BLE, plus an app "Relay" group.

#include <Arduino.h>
#include <garnet_web.h>

namespace {

constexpr int kRelayPin = 14; // free GPIO on the WT32-ETH01 header
constexpr const char *kRelay = "relay";
const char *const kBootStates[] = {"Off", "On", "Last state"};

String lastSwitched = "never";

void applyRelay() { digitalWrite(kRelayPin, gsGetBool(kRelay, "on") ? HIGH : LOW); }

void onRelayChanged(const GsField &) {
  applyRelay();
  lastSwitched = String(millis() / 1000) + " s after boot";
}

void pulse() {
  // Runs on the loop task (garnet_web defers button actions), so a short
  // blocking pulse here only pauses the loop, never the web server.
  digitalWrite(kRelayPin, HIGH);
  delay(gsGetInt(kRelay, "pulse"));
  applyRelay();
  lastSwitched = String(millis() / 1000) + " s after boot (pulse)";
}

String lastSwitchedStr() { return lastSwitched; }

const GsField kRelayFields[] = {
    gsToggle("on", "Relay on", false, onRelayChanged),
    gsWithHelp(gsNumber("pulse", "Pulse length (ms)", 500, 50, 5000, 50), "Closed time for Pulse"),
    gsSelect("boot", "State at power-up", kBootStates, 3, 0),
    gsButton("pulsebtn", "Pulse now", pulse, "Close the relay for the pulse length?"),
    gsInfo("last", "Last switched", lastSwitchedStr),
};
const GsGroup kRelayGroup = gsGroup(kRelay, "Relay", "power", kRelayFields);

// WT32-ETH01 wiring: LAN8720 at PHY address 1, MDC 23, MDIO 18, PHY power
// enable on GPIO16, 50 MHz RMII clock from the PHY into GPIO0.
const GwEthConfig kWt32Eth = {ETH_PHY_LAN8720, 1, 23, 18, 16, ETH_CLOCK_GPIO0_IN};

} // namespace

void setup() {
  Serial.begin(115200);
  pinMode(kRelayPin, OUTPUT);
  gsRegister(kRelayGroup);

  switch (gsGetInt(kRelay, "boot")) {
  case 0: gsSetBool(kRelay, "on", false); break;
  case 1: gsSetBool(kRelay, "on", true); break;
  default: break; // last state: keep what's stored
  }
  applyRelay();

  GwConfig cfg;
  cfg.name = "Relay Box";
  cfg.defaultPassword = "changeme";
  cfg.appVersion = "0.1.0";
  gwSetEth(kWt32Eth);
  gwBegin(cfg);
}

void loop() { gwLoop(); }
