#pragma once

// Shared between garnet_web's own .cpp files only - not part of the API.

#include "garnet_web.h"

#include <cJSON.h>
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

// Runs fn(ctx) on gwLoop's task and waits for it (up to timeoutMs) - for
// server requests that must touch hardware (I2C, GPIO, UART) without
// racing the app, which owns the hardware from its loop. ctx is copied in
// and out (ctxSize bytes), so a timed-out call can never leave the loop
// writing into a dead stack frame. False = timed out (gwLoop not called,
// or blocked) - the job then still runs later, on its own copy.
bool gwRunOnLoop(void (*fn)(void *ctx), void *ctx, size_t ctxSize, uint32_t timeoutMs);

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

// JSON response helpers shared by the feature modules (gw_time, gw_log...).
esp_err_t gwHttpJson(httpd_req_t *req, cJSON *json, const char *status = "200 OK");
esp_err_t gwHttpOk(httpd_req_t *req);
esp_err_t gwHttpError(httpd_req_t *req, const char *status, const String &error);
cJSON *gwHttpReadJson(httpd_req_t *req); // null = error already sent
String gwHttpJsonStr(const cJSON *obj, const char *key);
int gwHttpJsonInt(const cJSON *obj, const char *key, int fallback);
String gwHttpQuery(httpd_req_t *req, const char *key); // %-decoded, "" = absent

// Feature modules: return true when they handled `path` (already
// authenticated), with the send result in `err`.
#if defined(GARNET_WEB_TIME)
bool gwTimeHandle(httpd_req_t *req, const String &path, bool post, esp_err_t &err);
void gwTimeBegin();
void gwTimeRegister();
#endif
#if defined(GARNET_WEB_LOG)
bool gwLogHandle(httpd_req_t *req, const String &path, bool post, esp_err_t &err);
void gwLogBegin();
void gwLogLoop();
#endif
#if defined(GARNET_WEB_HW)
bool gwHwHandle(httpd_req_t *req, const String &path, bool post, esp_err_t &err);
void gwHwBegin();
#endif
#if defined(GARNET_WEB_UART)
bool gwUartHandle(httpd_req_t *req, const String &path, bool post, esp_err_t &err);
void gwUartBegin();
void gwUartLoop();
bool gwUartUsesPin(int pin); // hardware tools leave the monitor's pins alone
#endif

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

// ---- Files -------------------------------------------------------------------------
#if defined(GARNET_WEB_FILES)
// Handles every /api/fs* request (already authenticated).
esp_err_t gwFilesHandle(httpd_req_t *req, const String &path, bool post);
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
