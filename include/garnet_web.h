#pragma once

#include <Arduino.h>
#include <IPAddress.h>
#include <garnet_settings.h>

#if defined(GARNET_WEB_ETH)
#include <ETH.h>
#endif
#if defined(GARNET_WEB_SD)
#include <SD.h>
#endif

// garnet_web - an iPadOS-settings-style browser UI for ESP32 devices that
// also replaces WiFiManager. The left column lists every registered
// garnet_settings group, and the right column shows that group's fields,
// each group with its own Save button.
//
// Built-in groups are compiled in by build flag:
//   GARNET_WEB_WIFI  WiFi: scan + connect, several saved networks, DHCP/static,
//                    and a captive-portal AP when no network can be reached
//   GARNET_WEB_ETH   Ethernet (RMII PHY, e.g. the WT32-ETH01's LAN8720). It is the
//                    primary link: when it is up WiFi STA is off, when it drops WiFi takes over
//   GARNET_WEB_BT    Bluetooth: on/off + advertised name
//   GARNET_WEB_SD    SD card info (the app mounts the card and passes it in)
//   (always)         System: chip/memory/uptime info, hostname, web password,
//                    settings backup/restore
// App groups are plain gsRegister() calls and show up after the built-ins.
//
//   void setup() {
//     gsRegister(kRelayGroup);
//     GwConfig cfg;
//     cfg.name = "Relay";
//     cfg.defaultPassword = "changeme";
//     gwSetEth(kWt32Eth);   // before gwBegin
//     gwBegin(cfg);
//   }
//   void loop() { gwLoop(); }
//
// Threading: HTTP requests are served on esp_http_server's own task, but
// every app callback (GsField::onChange / action, GsGroup::onSave) is
// handed to gwLoop() and runs on the task that calls it, normally the
// Arduino loop task. App state therefore needs no locking. The one
// exception is GsField::info getters: they are called directly from the
// server task, so keep them read-only.

struct GwConfig {
  const char *name = "ESP32";           // UI title, AP SSID prefix, default hostname prefix
  const char *defaultPassword = nullptr; // required: web UI password until changed in the UI
  const char *appVersion = nullptr;      // shown in System, null = hidden
  const char *accent = nullptr;          // title-tag/selection color (CSS color), null = Garnet navy / sky blue
  const char *logoSvg = nullptr;         // inline <svg> shown above the sidebar, null = none
  uint16_t port = 80;
  // How long to try saved WiFi networks (or wait for an Ethernet link)
  // before opening the setup access point.
  uint32_t portalTimeoutMs = 30000;
};

#if defined(GARNET_WEB_ETH)
// Board wiring for an RMII PHY, passed straight to ETH.begin().
// WT32-ETH01: {ETH_PHY_LAN8720, 1, 23, 18, 16, ETH_CLOCK_GPIO0_IN}
struct GwEthConfig {
  eth_phy_type_t phy;
  int32_t addr;
  int mdc;
  int mdio;
  int power; // -1 = no power pin
  eth_clock_mode_t clk;
};
void gwSetEth(const GwEthConfig &cfg); // call before gwBegin
#endif

#if defined(GARNET_WEB_SD)
// The app owns SD init (pins, SPI bus, SD vs SD_MMC differ per board);
// garnet_web only reads card info from it. Call any time, even after
// gwBegin - the SD group shows "Not mounted" until then.
void gwSetSd(fs::SDFS &sd);
#endif

#if defined(GARNET_WEB_UI_FS)
// Serve the UI from a filesystem instead of flash: the file
// /garnet_web/index.html.gz (tools/build_ui.py --out-gz) on `fs`.
#include <FS.h>
void gwSetUiFs(fs::FS &fs);
#endif

void gwBegin(const GwConfig &cfg);
void gwLoop(); // call every loop(): net state machine, captive DNS, app callbacks, reboot

enum class GwNetMode : uint8_t {
  Offline,  // trying to connect
  Ethernet, // Ethernet up (WiFi STA off)
  WiFi,     // WiFi STA up
  Portal,   // setup access point + captive portal (no uplink)
};
GwNetMode gwNetMode();
IPAddress gwNetIP();     // current uplink address (AP address in Portal mode)
String gwHostname();     // effective hostname (what name.local resolves)
bool gwPortalActive();   // the setup AP is up (may overlap an uplink for a grace period)
