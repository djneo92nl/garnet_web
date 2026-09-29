#pragma once

// Shared between garnet_web's own .cpp files only - not part of the API.

#include "garnet_web.h"

#include <esp_http_server.h>

extern GwConfig gwCfg;

// ---- Deferred work (server task -> gwLoop task) ------------------------------
// App callbacks must run on the app's own task (see garnet_web.h), so the
// server only enqueues and returns.
void gwDeferInit();
void gwDeferChanged(const GsGroup *group, uint32_t mask);
void gwDeferAction(const GsField *field);
void gwDeferReboot(uint32_t delayMs);
void gwDeferLoop();

// ---- Shared state lock --------------------------------------------------------
// One mutex for the small bits of state both tasks touch (scan results,
// net status snapshot, sessions). Held only for copies, never across I/O.
void gwLock();
void gwUnlock();

// ---- Auth ---------------------------------------------------------------------------
constexpr size_t kGwTokenLen = 32; // hex chars
void gwAuthBegin();
bool gwAuthCheck(httpd_req_t *req);                  // valid session cookie?
bool gwAuthLogin(const String &password, String &tokenOut, uint32_t &retryAfterS);
void gwAuthLogout(httpd_req_t *req);
bool gwAuthChangePassword(const String &current, const String &next, String &why);

// ---- Server -------------------------------------------------------------------------
void gwServerBegin();

// ---- Net ------------------------------------------------------------------------------
void gwNetBegin();
void gwNetLoop();
IPAddress gwApIP();

#if defined(GARNET_WEB_WIFI)
constexpr uint8_t kGwMaxSavedNets = 5;
constexpr uint8_t kGwMaxScanNets = 24;
struct GwScanNet {
  char ssid[33];
  int8_t rssi;
  bool secure;
};
// Status snapshot + scan, as JSON, for GET /api/wifi.
void *gwWifiStatusJson();
void gwWifiRequestScan();                                  // runs on gwLoop's task
bool gwWifiAddNet(const String &ssid, const String &pass); // saves, caller reboots
bool gwWifiForgetNet(const String &ssid);
#endif

// ---- Built-in groups -------------------------------------------------------------
// Each registers its GsGroup (in sidebar order) when its flag is set.
void gwGroupsRegister();
#if defined(GARNET_WEB_BT)
void gwBtBegin(); // applies the saved BT settings at boot
#endif
#if defined(GARNET_WEB_SD)
void gwSdLoop(); // refreshes the cached card info on the app's task
#endif

// Namespace/field ids the net code reads, defined next to their tables.
extern const char *const kGwSysGroup;   // "system"
extern const char *const kGwWifiGroup;  // "wifi"
extern const char *const kGwEthGroup;   // "eth"

// Formatting helpers used by several Info getters.
String gwFmtBytes(uint64_t bytes);
String gwFmtUptime(uint32_t seconds);
