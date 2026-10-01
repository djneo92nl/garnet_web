#include "gw_internal.h"

#include <ESPmDNS.h>
#include <Network.h>
#include <Preferences.h>
#include <atomic>
#include <cJSON.h>
#include <esp_mac.h>

#if defined(GARNET_WEB_WIFI)
#include <DNSServer.h>
#include <WiFi.h>
#endif

// Uplink manager - the part that replaces WiFiManager.
//
//   Ethernet (if compiled in + configured) is primary: while it has an IP,
//   WiFi STA is off. Losing it (cable pulled) starts WiFi.
//   WiFi tries every saved network, strongest known one first; networks
//   not seen in the scan are tried last (they may be hidden SSIDs).
//   When nothing connects within portalTimeoutMs, a setup AP
//   "<name>-XXXX" with a captive portal comes up. It keeps retrying the
//   saved networks every kRetryMs while nobody is connected to the AP.
//   The AP closes once an uplink has been up for kPortalGraceMs and no
//   client is on it.
//
// Everything below runs on gwLoop's task. The network event callback
// only sets atomics, and the server task only reads a snapshot under
// gwLock (see gwWifiStatusJson).

namespace {

constexpr uint32_t kPortalGraceMs = 60000;

std::atomic<bool> ethGotIp{false};
bool mdnsStarted = false;
String hostname;
uint32_t bootMs = 0;
GwNetMode mode = GwNetMode::Offline;

#if defined(GARNET_WEB_ETH)
bool ethConfigured = false;
bool ethStarted = false;
GwEthConfig ethCfg;
#else
constexpr bool ethConfigured = false;
#endif

void onNetEvent(arduino_event_id_t event, arduino_event_info_t) {
  switch (event) {
  case ARDUINO_EVENT_ETH_GOT_IP: ethGotIp = true; break;
  case ARDUINO_EVENT_ETH_DISCONNECTED:
  case ARDUINO_EVENT_ETH_LOST_IP:
  case ARDUINO_EVENT_ETH_STOP: ethGotIp = false; break;
  default: break;
  }
}

// Last two bytes of the factory (efuse) MAC as "1E08". Read from efuse,
// not WiFi/Network.macAddress(): those return zeros until the driver has
// started, and the hostname / AP name are chosen before that.
String macSuffix() {
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_BASE);
  char buf[5];
  snprintf(buf, sizeof(buf), "%02X%02X", mac[4], mac[5]);
  return buf;
}

String defaultHostname() {
  // "<name>-xxxx": lowercase, [a-z0-9-] only, + the last 2 MAC bytes so
  // two identical devices on one LAN don't fight over name.local.
  String h;
  for (const char *c = gwCfg.name ? gwCfg.name : "esp32"; *c && h.length() < 24; c++) {
    char ch = tolower(*c);
    if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) h += ch;
    else if (h.length() && h[h.length() - 1] != '-') h += '-';
  }
  if (h.length() == 0) h = "esp32";
  String suffix = macSuffix();
  suffix.toLowerCase();
  return h + "-" + suffix;
}

bool parseIp(const String &s, IPAddress &out) { return gsValidIPv4(s) && out.fromString(s); }

void startMdnsOnce() {
  if (mdnsStarted) return;
  if (MDNS.begin(hostname.c_str())) {
    MDNS.addService("http", "tcp", gwCfg.port);
    mdnsStarted = true;
    log_i("garnet_web: http://%s.local/", hostname.c_str());
  }
}

// ---- Ethernet ------------------------------------------------------------------------

#if defined(GARNET_WEB_ETH)
void ethBegin() {
  if (!ethConfigured || ethStarted) return;
  if (!ETH.begin(ethCfg.phy, ethCfg.addr, ethCfg.mdc, ethCfg.mdio, ethCfg.power, ethCfg.clk)) {
    log_e("garnet_web: ETH.begin failed - check GwEthConfig pins");
    return;
  }
  ethStarted = true;
  // Arduino 3.x wants config() after begin() (it needs the netif).
  if (!gsGetBool(kGwEthGroup, "dhcp")) {
    IPAddress ip, gw, mask, dns;
    if (parseIp(gsGetString(kGwEthGroup, "ip"), ip) && parseIp(gsGetString(kGwEthGroup, "mask"), mask)) {
      parseIp(gsGetString(kGwEthGroup, "gw"), gw);
      if (!parseIp(gsGetString(kGwEthGroup, "dns"), dns)) dns = gw;
      ETH.config(ip, gw, mask, dns);
    } else {
      log_w("garnet_web: ETH static IP incomplete - falling back to DHCP");
    }
  }
}
#endif

// ---- WiFi -------------------------------------------------------------------------------

#if defined(GARNET_WEB_WIFI)

constexpr uint32_t kConnectTimeoutMs = 12000; // per saved network
constexpr uint32_t kRetryMs = 120000;         // portal: retry saved networks this often
constexpr uint32_t kPortalRetryMs = 2000;     // setup AP failed to start: try again after this
constexpr uint8_t kScanRetries = 6;
constexpr const char *kNetsNs = "gw_wifinets";

enum class Sta : uint8_t { Off, Scan, Connect, Connected, Wait };

struct SavedNet {
  String ssid;
  String pass;
};

Sta sta = Sta::Off;
uint32_t staSince = 0;
uint32_t disconnectedSince = 0;
uint8_t candidates[kGwMaxSavedNets];
uint8_t candidateCount = 0;
uint8_t candidateIdx = 0;

SavedNet saved[kGwMaxSavedNets];
uint8_t savedCount = 0;

// Scan: one async scan at a time, shared by the connect logic and the UI.
std::atomic<bool> scanRequested{false};
bool scanRunning = false;
bool scanForConnect = false;
uint8_t scanRetries = 0;
uint32_t scanRetryAt = 0;
GwScanNet scanNets[kGwMaxScanNets]; // guarded by gwLock
uint8_t scanCount = 0;              // guarded by gwLock
bool scanDone = false;              // guarded by gwLock

// Portal
DNSServer dns;
bool portalOn = false;
uint32_t uplinkSince = 0; // millis when an uplink came up, 0 = none
uint32_t downSince = 0;   // millis since there has been no uplink
String apSsid;

// Snapshot for the server task (guarded by gwLock).
struct StaSnapshot {
  bool connected = false;
  String ssid;
  String ip;
  int rssi = 0;
} snap;

void loadSaved() {
  savedCount = 0;
  Preferences p;
  if (!gsNamespaceExists(kNetsNs) || !p.begin(kNetsNs, true)) return;
  for (uint8_t i = 0; i < kGwMaxSavedNets; i++) {
    char k[4] = {'s', char('0' + i), 0};
    if (!p.isKey(k)) break;
    saved[savedCount].ssid = p.getString(k);
    k[0] = 'p';
    saved[savedCount].pass = p.getString(k, "");
    savedCount++;
  }
  p.end();
}

bool storeSaved() {
  Preferences p;
  if (!p.begin(kNetsNs, false)) return false;
  p.clear();
  bool ok = true;
  for (uint8_t i = 0; i < savedCount; i++) {
    char k[4] = {'s', char('0' + i), 0};
    p.putString(k, saved[i].ssid);
    k[0] = 'p';
    p.putString(k, saved[i].pass);
    ok = ok && p.isKey(k);
  }
  p.end();
  return ok;
}

void startScan(bool forConnect) {
  // Async so gwLoop (and whatever UI the app draws in loop) keeps running;
  // show_hidden=false, passive=false.
  WiFi.scanDelete();
  int16_t r = WiFi.scanNetworks(true, false);
  scanForConnect = forConnect;
  if (r == WIFI_SCAN_FAILED) {
    // Same race garnet_ui's WiFi selector hit on hardware: a scan landing
    // while a WiFi.begin() is still resolving is refused with
    // ESP_ERR_WIFI_STATE. Retry after a settling delay instead of failing.
    scanRunning = false;
    scanRetryAt = millis() + 400;
    return;
  }
  scanRunning = true;
  scanRetryAt = 0;
  gwLock();
  scanDone = false;
  gwUnlock();
}

// Returns true once a scan has finished (successfully or given up).
bool pollScan() {
  if (!scanRunning) {
    if (scanRetryAt && (int32_t)(millis() - scanRetryAt) >= 0) {
      if (++scanRetries > kScanRetries) {
        scanRetries = 0;
        scanRetryAt = 0;
        gwLock();
        scanCount = 0;
        scanDone = true;
        gwUnlock();
        return true;
      }
      WiFi.disconnect();
      startScan(scanForConnect);
    }
    return false;
  }
  int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return false;
  scanRunning = false;
  scanRetries = 0;
  if (n < 0) n = 0;

  gwLock();
  scanCount = 0;
  for (int16_t i = 0; i < n && scanCount < kGwMaxScanNets; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue; // hidden network, nothing to show
    // Same SSID from several APs (mesh): keep the strongest only.
    bool dup = false;
    for (uint8_t j = 0; j < scanCount; j++) {
      if (s == scanNets[j].ssid) {
        if (WiFi.RSSI(i) > scanNets[j].rssi) scanNets[j].rssi = WiFi.RSSI(i);
        dup = true;
        break;
      }
    }
    if (dup) continue;
    GwScanNet &net = scanNets[scanCount++];
    strlcpy(net.ssid, s.c_str(), sizeof(net.ssid));
    net.rssi = WiFi.RSSI(i);
    net.secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  // Strongest first - what the UI list and the connect order both want.
  for (uint8_t a = 1; a < scanCount; a++) {
    for (uint8_t b = a; b > 0 && scanNets[b].rssi > scanNets[b - 1].rssi; b--) {
      GwScanNet t = scanNets[b];
      scanNets[b] = scanNets[b - 1];
      scanNets[b - 1] = t;
    }
  }
  scanDone = true;
  gwUnlock();
  WiFi.scanDelete();
  return true;
}

void buildCandidates() {
  candidateCount = 0;
  candidateIdx = 0;
  bool used[kGwMaxSavedNets] = {};
  gwLock();
  for (uint8_t i = 0; i < scanCount; i++) { // scanNets is sorted strongest first
    for (uint8_t s = 0; s < savedCount; s++) {
      if (!used[s] && saved[s].ssid == scanNets[i].ssid) {
        used[s] = true;
        candidates[candidateCount++] = s;
      }
    }
  }
  gwUnlock();
  for (uint8_t s = 0; s < savedCount; s++) { // not seen: maybe hidden, try last
    if (!used[s]) candidates[candidateCount++] = s;
  }
}

void applyStaticIp() {
  if (gsGetBool(kGwWifiGroup, "dhcp")) return;
  IPAddress ip, gw, mask, dnsIp;
  if (parseIp(gsGetString(kGwWifiGroup, "ip"), ip) && parseIp(gsGetString(kGwWifiGroup, "mask"), mask)) {
    parseIp(gsGetString(kGwWifiGroup, "gw"), gw);
    if (!parseIp(gsGetString(kGwWifiGroup, "dns"), dnsIp)) dnsIp = gw;
    WiFi.config(ip, gw, mask, dnsIp);
  } else {
    log_w("garnet_web: WiFi static IP incomplete - falling back to DHCP");
  }
}

void setSta(Sta next) {
  sta = next;
  staSince = millis();
}

void connectCandidate() {
  gwLock(); // saved[] can be rewritten by the server task (add / forget)
  SavedNet n = saved[candidates[candidateIdx]];
  gwUnlock();
  log_i("garnet_web: connecting to '%s'", n.ssid.c_str());
  WiFi.disconnect();
  WiFi.begin(n.ssid.c_str(), n.pass.length() ? n.pass.c_str() : nullptr);
  setSta(Sta::Connect);
}

void portalStart() {
  if (portalOn) return;
  // softAP() can fail (see below); don't hammer it from every loop pass.
  static uint32_t lastTry = 0;
  if (lastTry != 0 && millis() - lastTry < kPortalRetryMs) return;
  lastTry = millis();

  apSsid = String(gwCfg.name ? gwCfg.name : "ESP32") + "-" + macSuffix();
  // With a saved network out of range the driver keeps rejoining it on its
  // own, scanning every channel and dragging the AP along: phones then see
  // the AP but can't finish joining. The state machine retries saved
  // networks itself (kRetryMs, only while nobody is on the AP), so the
  // driver's own reconnect stays off until an uplink is back (gwNetLoop).
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false);
  // AP+STA: STA stays available for scanning and the periodic retry.
  WiFi.mode(WIFI_AP_STA);
  // Open AP by design: the web UI password guards the settings, and an
  // AP password printed nowhere would just lock the owner out.
  // softAP() fails when the STA side is still busy with a join attempt,
  // which happens when the portal opens right after a saved network
  // failed. Leave portalOn false then, so the next pass retries.
  if (!WiFi.softAP(apSsid.c_str())) {
    log_e("garnet_web: starting setup AP '%s' failed, retrying", apSsid.c_str());
    return;
  }
  dns.start(53, "*", WiFi.softAPIP());
  portalOn = true;
  log_i("garnet_web: setup AP '%s' at http://%s/", apSsid.c_str(),
        WiFi.softAPIP().toString().c_str());
}

void portalStop() {
  if (!portalOn) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  portalOn = false;
  log_i("garnet_web: setup AP closed");
}

void staOff() {
  if (sta == Sta::Off) return;
  WiFi.disconnect(true);
  if (scanRunning) {
    WiFi.scanDelete();
    scanRunning = false;
  }
  setSta(Sta::Off);
  if (!portalOn) WiFi.mode(WIFI_OFF);
}

void staOn() {
  if (sta != Sta::Off) return;
  WiFi.mode(portalOn ? WIFI_AP_STA : WIFI_STA);
  applyStaticIp();
  if (savedCount > 0) {
    startScan(true);
    setSta(Sta::Scan);
  } else {
    setSta(Sta::Wait);
  }
}

// One step of the STA state machine. Only called while STA should be on.
void staLoop() {
  uint32_t now = millis();
  switch (sta) {
  case Sta::Off: staOn(); break;
  case Sta::Scan:
    if (pollScan()) {
      buildCandidates();
      if (candidateCount) connectCandidate();
      else setSta(Sta::Wait);
    }
    break;
  case Sta::Connect:
    if (WiFi.status() == WL_CONNECTED) {
      setSta(Sta::Connected);
      disconnectedSince = 0;
      log_i("garnet_web: WiFi up, %s", WiFi.localIP().toString().c_str());
    } else if (now - staSince > kConnectTimeoutMs) {
      if (++candidateIdx < candidateCount) connectCandidate();
      else setSta(Sta::Wait);
    }
    break;
  case Sta::Connected:
    if (WiFi.status() == WL_CONNECTED) {
      disconnectedSince = 0;
    } else {
      // Brief drops are the driver's autoReconnect's job; only rescan
      // (maybe to a different saved network) after a sustained outage.
      if (disconnectedSince == 0) disconnectedSince = now;
      if (now - disconnectedSince > gwCfg.portalTimeoutMs) {
        startScan(true);
        setSta(Sta::Scan);
      }
    }
    break;
  case Sta::Wait:
    // Nothing connected. Open the portal once the budget is spent (or at
    // once when there is nothing to try), then retry now and then - but
    // never while someone is on the AP: the retry hops the radio off the
    // AP's channel, and would drop them mid-setup.
    if (savedCount > 0 && portalOn && now - staSince > kRetryMs && WiFi.softAPgetStationNum() == 0) {
      startScan(true);
      setSta(Sta::Scan);
    }
    break;
  }

  // A UI scan request runs whenever no connect attempt is in flight.
  if (scanRequested && !scanRunning && !scanRetryAt && sta != Sta::Connect) {
    scanRequested = false;
    startScan(false);
  }
  if (!scanForConnect && (scanRunning || scanRetryAt)) pollScan();
}

#endif // GARNET_WEB_WIFI

bool uplinkUp() {
#if defined(GARNET_WEB_WIFI)
  if (sta == Sta::Connected && WiFi.status() == WL_CONNECTED) return true;
#endif
  return ethGotIp;
}

} // namespace

// ---- Public ------------------------------------------------------------------------------

#if defined(GARNET_WEB_ETH)
void gwSetEth(const GwEthConfig &cfg) {
  ethCfg = cfg;
  ethConfigured = true;
}
#endif

String gwHostname() { return hostname; }

GwNetMode gwNetMode() { return mode; }

IPAddress gwNetIP() {
#if defined(GARNET_WEB_ETH)
  if (ethGotIp) return ETH.localIP();
#endif
#if defined(GARNET_WEB_WIFI)
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP();
  if (portalOn) return WiFi.softAPIP();
#endif
  return IPAddress();
}

IPAddress gwApIP() {
#if defined(GARNET_WEB_WIFI)
  return WiFi.softAPIP();
#else
  return IPAddress();
#endif
}

bool gwPortalActive() {
#if defined(GARNET_WEB_WIFI)
  return portalOn;
#else
  return false;
#endif
}

void gwNetBegin() {
  bootMs = millis();
  Network.begin();
  hostname = gsGetString(kGwSysGroup, "hostname");
  if (hostname.length() == 0) hostname = defaultHostname();
  // Must precede every interface start: the hostname is copied into each
  // netif as it is created, and is not updated afterwards.
  Network.setHostname(hostname.c_str());
  Network.onEvent(onNetEvent);

#if defined(GARNET_WEB_WIFI)
  // We keep our own credentials list - stop the driver keeping a second
  // copy in its NVS that would auto-connect behind our back at boot.
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  loadSaved();
#endif
#if defined(GARNET_WEB_ETH)
  ethBegin();
#endif
}

void gwNetLoop() {
  uint32_t now = millis();

#if defined(GARNET_WEB_WIFI)
  // Give Ethernet the first portalTimeoutMs to come up before touching
  // WiFi at all when it's the configured primary - otherwise a WT32 on a
  // cable would briefly join WiFi (and grab a second IP) at every boot.
  bool ethGrace = ethConfigured && !ethGotIp && now - bootMs < gwCfg.portalTimeoutMs;
  if (ethGotIp) {
    staOff();
  } else if (!ethGrace) {
    staLoop();
    // Portal when there is nothing to try, every saved network failed, or
    // the time budget ran out while still attempting them.
    // The AP can drop while the portal counts as open (e.g. around a retry
    // scan): notice it, so portalStart() brings it back.
    static uint32_t lastApCheck = 0;
    if (portalOn && now - lastApCheck > 1000) {
      lastApCheck = now;
      if (WiFi.softAPSSID().isEmpty()) {
        log_w("garnet_web: setup AP '%s' went down, restarting", apSsid.c_str());
        dns.stop();
        portalOn = false;
      }
    }
    bool spent = savedCount == 0 || sta == Sta::Wait ||
                 (downSince != 0 && now - downSince > gwCfg.portalTimeoutMs);
    if (!portalOn && spent) portalStart();
  }

  // User scan while STA is off (Ethernet up): still allow it, so a
  // wired device can have WiFi prepared as a fallback.
  if (sta == Sta::Off && scanRequested) {
    WiFi.mode(WIFI_STA);
    scanRequested = false;
    startScan(false);
  }
  if (sta == Sta::Off && (scanRunning || scanRetryAt)) {
    if (pollScan() && !portalOn) WiFi.mode(WIFI_OFF);
  }

  if (uplinkUp()) {
    downSince = 0;
#if defined(GARNET_WEB_WIFI)
    // portalStart() turned it off; brief drops are its job again (Sta::Connected)
    if (!WiFi.getAutoReconnect()) WiFi.setAutoReconnect(true);
#endif
    if (uplinkSince == 0) uplinkSince = now;
    if (portalOn && now - uplinkSince > kPortalGraceMs && WiFi.softAPgetStationNum() == 0) {
      portalStop();
    }
  } else {
    uplinkSince = 0;
    if (downSince == 0) downSince = now;
  }

  gwLock();
  snap.connected = WiFi.status() == WL_CONNECTED;
  if (snap.connected) {
    snap.ssid = WiFi.SSID();
    snap.ip = WiFi.localIP().toString();
    snap.rssi = WiFi.RSSI();
  } else {
    snap.ssid = "";
    snap.ip = "";
    snap.rssi = 0;
  }
  gwUnlock();
#endif

  if (ethGotIp) mode = GwNetMode::Ethernet;
#if defined(GARNET_WEB_WIFI)
  else if (WiFi.status() == WL_CONNECTED) mode = GwNetMode::WiFi;
  else if (portalOn) mode = GwNetMode::Portal;
#endif
  else mode = GwNetMode::Offline;

  if (mode == GwNetMode::Ethernet || mode == GwNetMode::WiFi) startMdnsOnce();
}

// ---- WiFi API for the server + WiFi group ----------------------------------------

#if defined(GARNET_WEB_WIFI)

void gwWifiRequestScan() { scanRequested = true; }

void *gwWifiStatusJson() {
  cJSON *o = cJSON_CreateObject();
  if (o == nullptr) return nullptr;
  const char *m = "offline";
  switch (mode) {
  case GwNetMode::Ethernet: m = "ethernet"; break;
  case GwNetMode::WiFi: m = "wifi"; break;
  case GwNetMode::Portal: m = "portal"; break;
  default: break;
  }
  cJSON_AddStringToObject(o, "mode", m);
  cJSON_AddBoolToObject(o, "portal", portalOn);
  if (portalOn) cJSON_AddStringToObject(o, "apSsid", apSsid.c_str());

  gwLock();
  cJSON_AddBoolToObject(o, "connected", snap.connected);
  cJSON_AddStringToObject(o, "ssid", snap.ssid.c_str());
  cJSON_AddStringToObject(o, "ip", snap.ip.c_str());
  cJSON_AddNumberToObject(o, "rssi", snap.rssi);

  cJSON *sv = cJSON_AddArrayToObject(o, "saved");
  for (uint8_t i = 0; sv && i < savedCount; i++) {
    cJSON_AddItemToArray(sv, cJSON_CreateString(saved[i].ssid.c_str()));
  }
  cJSON *scan = cJSON_AddObjectToObject(o, "scan");
  cJSON_AddStringToObject(scan, "state", scanRequested || scanRunning || scanRetryAt ? "running"
                                         : scanDone                                  ? "done"
                                                                                     : "idle");
  cJSON *nets = cJSON_AddArrayToObject(scan, "nets");
  for (uint8_t i = 0; nets && i < scanCount; i++) {
    cJSON *n = cJSON_CreateObject();
    cJSON_AddStringToObject(n, "ssid", scanNets[i].ssid);
    cJSON_AddNumberToObject(n, "rssi", scanNets[i].rssi);
    cJSON_AddBoolToObject(n, "secure", scanNets[i].secure);
    cJSON_AddItemToArray(nets, n);
  }
  gwUnlock();
  return o;
}

bool gwWifiAddNet(const String &ssid, const String &pass) {
  gwLock();
  int existing = -1;
  for (uint8_t i = 0; i < savedCount; i++) {
    if (saved[i].ssid == ssid) existing = i;
  }
  if (existing < 0) {
    if (savedCount == kGwMaxSavedNets) {
      // Full: forget the oldest entry (index 0) to make room.
      for (uint8_t i = 1; i < savedCount; i++) saved[i - 1] = saved[i];
      savedCount--;
    }
    existing = savedCount++;
    saved[existing].ssid = ssid;
  }
  saved[existing].pass = pass;
  bool ok = storeSaved();
  gwUnlock();
  return ok;
}

uint8_t gwWifiSavedSsids(String out[], uint8_t max) {
  gwLock();
  uint8_t n = savedCount < max ? savedCount : max;
  for (uint8_t i = 0; i < n; i++) out[i] = saved[i].ssid;
  gwUnlock();
  return n;
}

bool gwWifiForgetNet(const String &ssid) {
  gwLock();
  bool found = false;
  for (uint8_t i = 0; i < savedCount; i++) {
    if (!found && saved[i].ssid == ssid) found = true;
    if (found && i + 1 < savedCount) saved[i] = saved[i + 1];
  }
  if (found) {
    savedCount--;
    storeSaved();
  }
  gwUnlock();
  return found;
}

#endif // GARNET_WEB_WIFI
