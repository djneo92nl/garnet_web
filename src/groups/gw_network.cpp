#include "../gw_internal.h"

// WiFi and Ethernet groups. Both carry the same DHCP / static-IP block;
// gw_net reads these values once at boot (network groups reboot on save),
// so nothing here needs an onChange hook.

const char *const kGwWifiGroup = "wifi";
const char *const kGwEthGroup = "eth";

#if defined(GARNET_WEB_WIFI)
#include <WiFi.h>

namespace {

String wifiStatusStr() {
  switch (gwNetMode()) {
  case GwNetMode::WiFi: return "Connected";
  case GwNetMode::Ethernet: return "Off (Ethernet active)";
  case GwNetMode::Portal: return "Setup access point";
  default: return "Connecting...";
  }
}
String wifiSsidStr() { return WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("-"); }
String wifiIpStr() {
  return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("-");
}
String wifiRssiStr() {
  if (WiFi.status() != WL_CONNECTED) return "-";
  int r = WiFi.RSSI();
  const char *q = r > -55 ? "excellent" : r > -67 ? "good" : r > -75 ? "fair" : "weak";
  return String(r) + " dBm (" + q + ")";
}

const GsField kWifiFields[] = {
    gsInfo("status", "Status", wifiStatusStr),
    gsInfo("ssid", "Network", wifiSsidStr),
    gsInfo("ipnow", "IP address", wifiIpStr),
    gsInfo("rssi", "Signal", wifiRssiStr),
    gsToggle("dhcp", "Automatic IP (DHCP)", true),
    gsWithShowIf(gsWithPlaceholder(gsWithValidate(gsText("ip", "IP address", "", 15), gsValidIPv4OrEmpty, "not a valid IPv4 address"), "192.168.1.50"), "!dhcp"),
    gsWithShowIf(gsWithPlaceholder(gsWithValidate(gsText("mask", "Subnet mask", "255.255.255.0", 15), gsValidIPv4OrEmpty, "not a valid IPv4 address"), "255.255.255.0"), "!dhcp"),
    gsWithShowIf(gsWithPlaceholder(gsWithValidate(gsText("gw", "Router", "", 15), gsValidIPv4OrEmpty, "not a valid IPv4 address"), "192.168.1.1"), "!dhcp"),
    gsWithShowIf(gsWithPlaceholder(gsWithValidate(gsText("dns", "DNS", "", 15), gsValidIPv4OrEmpty, "not a valid IPv4 address"), "same as router"), "!dhcp"),
};

const GsGroup kWifi = gsWithWidget(
    gsWithReboot(gsGroup(kGwWifiGroup, "Wi-Fi", "wifi", kWifiFields, "Connections")), "wifi");

} // namespace

void gwGroupsRegisterWifi() { gsRegister(kWifi); }
#endif

#if defined(GARNET_WEB_ETH)
namespace {

String ethLinkStr() {
  if (!ETH.linkUp()) return "No cable";
  return String(ETH.linkSpeed()) + " Mbps, " + (ETH.fullDuplex() ? "full" : "half") + " duplex";
}
String ethIpStr() { return gwNetMode() == GwNetMode::Ethernet ? ETH.localIP().toString() : String("-"); }
String ethMacStr() { return ETH.macAddress(); }

const GsField kEthFields[] = {
    gsInfo("link", "Link", ethLinkStr),
    gsInfo("ipnow", "IP address", ethIpStr),
    gsInfo("mac", "MAC address", ethMacStr),
    gsToggle("dhcp", "Automatic IP (DHCP)", true),
    gsWithShowIf(gsWithPlaceholder(gsWithValidate(gsText("ip", "IP address", "", 15), gsValidIPv4OrEmpty, "not a valid IPv4 address"), "192.168.1.50"), "!dhcp"),
    gsWithShowIf(gsWithPlaceholder(gsWithValidate(gsText("mask", "Subnet mask", "255.255.255.0", 15), gsValidIPv4OrEmpty, "not a valid IPv4 address"), "255.255.255.0"), "!dhcp"),
    gsWithShowIf(gsWithPlaceholder(gsWithValidate(gsText("gw", "Router", "", 15), gsValidIPv4OrEmpty, "not a valid IPv4 address"), "192.168.1.1"), "!dhcp"),
    gsWithShowIf(gsWithPlaceholder(gsWithValidate(gsText("dns", "DNS", "", 15), gsValidIPv4OrEmpty, "not a valid IPv4 address"), "same as router"), "!dhcp"),
};

const GsGroup kEth =
    gsWithReboot(gsGroup(kGwEthGroup, "Ethernet", "ethernet", kEthFields, "Connections"));

} // namespace

void gwGroupsRegisterEth() { gsRegister(kEth); }
#endif
