#include "gw_internal.h"

#if defined(GARNET_WEB_TIME)

#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>

// Clock group: NTP + time zone, applied live (no reboot - SNTP and TZ are
// just state, nothing to tear down). A board on its own setup AP has no
// internet and so no NTP, which is exactly when a stored board is
// usually looked at - hence "Use browser time" (POST /api/time), which
// sets the clock from the viewing phone/laptop.
//
//   POST /api/time {epoch}   set the clock (seconds, UTC)

namespace {

constexpr const char *kGroup = "time";

// Labels + POSIX TZ strings. POSIX rules, not IANA names: newlib's
// tzset() only understands the former, and a table of the common ones
// is much smaller than a zone database.
const char *const kZoneLabels[] = {
    "UTC",
    "London, Lisbon",
    "Amsterdam, Berlin, Paris",
    "Athens, Helsinki, Kyiv",
    "Moscow, Istanbul",
    "Dubai",
    "India",
    "Bangkok, Jakarta",
    "China, Singapore, Perth",
    "Japan, Korea",
    "Sydney, Melbourne",
    "Auckland",
    "Honolulu",
    "Anchorage",
    "Los Angeles, Vancouver",
    "Denver",
    "Phoenix",
    "Chicago, Mexico City",
    "New York, Toronto",
    "Sao Paulo, Buenos Aires",
};
const char *const kZoneRules[] = {
    "UTC0",
    "GMT0BST,M3.5.0/1,M10.5.0",
    "CET-1CEST,M3.5.0,M10.5.0/3",
    "EET-2EEST,M3.5.0/3,M10.5.0/4",
    "<+03>-3",
    "<+04>-4",
    "IST-5:30",
    "<+07>-7",
    "<+08>-8",
    "<+09>-9",
    "AEST-10AEDT,M10.1.0,M4.1.0/3",
    "NZST-12NZDT,M9.5.0,M4.1.0/3",
    "HST10",
    "AKST9AKDT,M3.2.0,M11.1.0",
    "PST8PDT,M3.2.0,M11.1.0",
    "MST7MDT,M3.2.0,M11.1.0",
    "MST7",
    "CST6CDT,M3.2.0,M11.1.0",
    "EST5EDT,M3.2.0,M11.1.0",
    "<-03>3",
};
constexpr uint8_t kZoneCount = sizeof(kZoneLabels) / sizeof(kZoneLabels[0]);
static_assert(kZoneCount == sizeof(kZoneRules) / sizeof(kZoneRules[0]), "zone tables differ");

volatile time_t lastSync = 0;      // set by the SNTP callback (another task)
volatile bool setFromBrowser = false;

bool clockValid() { return time(nullptr) > 1700000000; } // after Nov 2023

void onSntpSync(struct timeval *) {
  lastSync = time(nullptr);
  setFromBrowser = false;
}

void applyClock() {
  int zone = gsGetInt(kGroup, "tz");
  setenv("TZ", kZoneRules[zone >= 0 && zone < kZoneCount ? zone : 0], 1);
  tzset();
  if (esp_sntp_enabled()) esp_sntp_stop();
  if (gsGetBool(kGroup, "ntp")) {
    String server = gsGetString(kGroup, "server");
    if (server.length() == 0) server = "pool.ntp.org";
    // configTzTime would re-set TZ from a string it must keep alive; we
    // already set it above, so plain configTime + our own TZ is enough.
    static String keep; // SNTP keeps the pointer, not a copy
    keep = server;
    configTime(0, 0, keep.c_str());
    sntp_set_time_sync_notification_cb(onSntpSync);
    // configTime resets TZ to UTC - put the chosen zone back.
    setenv("TZ", kZoneRules[zone >= 0 && zone < kZoneCount ? zone : 0], 1);
    tzset();
  }
}

void onClockSaved(const GsGroup &) { applyClock(); }

String nowStr() {
  if (!clockValid()) return "Not set";
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &tm);
  return buf;
}

String syncStr() {
  if (lastSync != 0) {
    return "NTP, " + gwFmtUptime((uint32_t)(time(nullptr) - lastSync)) + " ago";
  }
  if (setFromBrowser) return "Set from browser";
  return gsGetBool(kGroup, "ntp") ? "Waiting for NTP" : "Manual";
}

const GsField kFields[] = {
    gsInfo("now", "Local time", nowStr),
    gsInfo("sync", "Source", syncStr),
    gsSelect("tz", "Time zone", kZoneLabels, kZoneCount, 0),
    gsToggle("ntp", "Internet time (NTP)", true),
    gsWithShowIf(gsWithPlaceholder(gsText("server", "NTP server", "", 64), "pool.ntp.org"), "ntp"),
};

const GsGroup kClockGroup =
    gsWithWidget(gsWithOnSave(gsGroup(kGroup, "Clock", "clock", kFields, "Device"), onClockSaved),
                 "time");

} // namespace

// Registered from gwGroupsRegister (not gwTimeBegin) so Clock sits
// before System in the sidebar's Device section.
void gwTimeRegister() { gsRegister(kClockGroup); }

void gwTimeBegin() { applyClock(); }

bool gwTimeHandle(httpd_req_t *req, const String &path, bool post, esp_err_t &err) {
  if (!(post && path == "/api/time")) return false;
  cJSON *body = gwHttpReadJson(req);
  if (body == nullptr) {
    err = ESP_OK;
    return true;
  }
  const cJSON *e = cJSON_GetObjectItemCaseSensitive(body, "epoch");
  double epoch = cJSON_IsNumber(e) ? e->valuedouble : 0;
  cJSON_Delete(body);
  if (epoch < 1700000000) {
    err = gwHttpError(req, "422 Unprocessable Entity", "Invalid time");
    return true;
  }
  struct timeval tv = {(time_t)epoch, 0};
  settimeofday(&tv, nullptr);
  setFromBrowser = true;
  err = gwHttpOk(req);
  return true;
}

#endif // GARNET_WEB_TIME
