#include "../gw_internal.h"

#include <esp_arduino_version.h>
#include <esp_system.h>
#if defined(GARNET_WEB_OTA)
#include <esp_ota_ops.h>
#endif
#include "../gw_coproc.h"

// System group - always compiled in. Everything a "what is this device
// and how is it doing" page needs, plus the hostname (shared by WiFi and
// Ethernet, so it lives here rather than in either). The web password
// and backup/restore are drawn by the UI's "system" widget, not fields:
// they need flows (current password, file upload) a field can't express.

const char *const kGwSysGroup = "system";

namespace {

String chipStr() {
  return String(ESP.getChipModel()) + " rev " + String(ESP.getChipRevision()) + ", " +
         String(ESP.getChipCores()) + (ESP.getChipCores() == 1 ? " core" : " cores") + " @ " +
         String(ESP.getCpuFreqMHz()) + " MHz";
}
String flashStr() { return gwFmtBytes(ESP.getFlashChipSize()); }
String heapStr() {
  return gwFmtBytes(ESP.getFreeHeap()) + " free of " + gwFmtBytes(ESP.getHeapSize());
}
String heapMinStr() { return gwFmtBytes(ESP.getMinFreeHeap()); }
String heapBlockStr() { return gwFmtBytes(ESP.getMaxAllocHeap()); }
String psramStr() {
  if (ESP.getPsramSize() == 0) return "None";
  return gwFmtBytes(ESP.getFreePsram()) + " free of " + gwFmtBytes(ESP.getPsramSize());
}
String uptimeStr() { return gwFmtUptime(millis() / 1000); }

String resetStr() {
  switch (esp_reset_reason()) {
  case ESP_RST_POWERON: return "Power on";
  case ESP_RST_EXT: return "External pin";
  case ESP_RST_SW: return "Software restart";
  case ESP_RST_PANIC: return "Crash (panic)";
  case ESP_RST_INT_WDT: return "Interrupt watchdog";
  case ESP_RST_TASK_WDT: return "Task watchdog";
  case ESP_RST_WDT: return "Watchdog";
  case ESP_RST_DEEPSLEEP: return "Deep sleep wake";
  case ESP_RST_BROWNOUT: return "Brownout";
  case ESP_RST_SDIO: return "SDIO";
  default: return "Unknown";
  }
}

String versionStr() { return gwCfg.appVersion ? gwCfg.appVersion : "-"; }
String sdkStr() { return String("Arduino ") + ESP_ARDUINO_VERSION_STR + ", IDF " + ESP.getSdkVersion(); }

String macStr() {
  uint64_t m = ESP.getEfuseMac();
  char buf[18];
  // getEfuseMac() is little-endian: byte 0 of the MAC is the low byte.
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", (uint8_t)m, (uint8_t)(m >> 8),
           (uint8_t)(m >> 16), (uint8_t)(m >> 24), (uint8_t)(m >> 32), (uint8_t)(m >> 40));
  return buf;
}

String chipIdStr() { return gwChipId(); }

#if defined(GARNET_WEB_OTA)
// Which of the two OTA slots is running, and how big a firmware may be.
String slotStr() {
  const esp_partition_t *run = esp_ota_get_running_partition();
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  if (run == nullptr) return "-";
  return String(run->label) + ", max " + gwFmtBytes(next ? next->size : run->size);
}
#endif

#if GW_COPROC
// Wi-Fi co-processor firmware vs what this build expects. Only asked while
// the SDIO link is up (Wi-Fi active, or during an update) - otherwise every
// RPC would fail and log. Cached: it's an RPC round trip over SDIO, and the
// page polls Info rows every 2 s.
String coprocStr() {
  uint32_t hM, hm, hp;
  hostedGetHostVersion(&hM, &hm, &hp);
  String host = String(hM) + "." + String(hm) + "." + String(hp);
  if (!hostedIsInitialized()) return "Link off (Ethernet active), host v" + host;
  static String cached;
  if (cached.length()) return cached;
  uint32_t M = 0, m = 0, p = 0;
  hostedGetSlaveVersion(&M, &m, &p);
  if (M == 0 && m == 0 && p == 0) return "Older firmware (no version report), host v" + host;
  String slave = String(M) + "." + String(m) + "." + String(p);
  cached = "ESP-Hosted v" + slave + (slave == host ? String(" (matches host)") : ", host v" + host);
  return cached;
}
#endif

String hostnameStr() { return gwHostname() + ".local"; }

bool hostnameOrEmpty(const String &v) { return v.length() == 0 || gsValidHostname(v); }

const GsField kFields[] = {
    gsWithHelp(gsWithPlaceholder(gsWithValidate(gsText("hostname", "Hostname", "", 32),
                                                hostnameOrEmpty, "letters, digits and - only"),
                                 "automatic"),
               "Empty = automatic"),
    gsInfo("mdns", "Address", hostnameStr),
    gsInfo("version", "Firmware", versionStr),
    gsInfo("chip", "Chip", chipStr),
    gsInfo("chipid", "Chip ID", chipIdStr),
    gsInfo("flash", "Flash", flashStr),
    gsInfo("heap", "Memory", heapStr),
    gsInfo("heapmin", "Lowest free memory", heapMinStr),
    gsInfo("heapblk", "Largest free block", heapBlockStr),
    gsInfo("psram", "PSRAM", psramStr),
    gsInfo("uptime", "Uptime", uptimeStr),
    gsInfo("reset", "Last reset", resetStr),
    gsInfo("mac", "MAC address", macStr),
    gsInfo("sdk", "Software", sdkStr),
#if defined(GARNET_WEB_OTA)
    gsInfo("slot", "Firmware slot", slotStr),
#endif
#if GW_COPROC
    gsInfo("coproc", "Wi-Fi co-processor", coprocStr),
#endif
};

const GsGroup kGroup =
    gsWithWidget(gsWithReboot(gsGroup(kGwSysGroup, "System", "system", kFields, "Device")), "system");

} // namespace

// Sidebar order = registration order: connections first, then the
// device groups. App groups registered before gwBegin() still sit
// between them - the UI orders by section ("Connections" first,
// "Device" last, app sections in between).
void gwGroupsRegisterWifi();
void gwGroupsRegisterEth();
void gwGroupsRegisterBt();
void gwGroupsRegisterSd();

#if defined(GARNET_WEB_FILES)
namespace {
// Fields-less group: the "files" widget is the whole page. Built by hand
// because gsGroup() needs a non-empty array.
GsGroup filesGroup() {
  GsGroup g{};
  g.id = "files";
  g.title = "Files";
  g.icon = "folder";
  g.section = "Device";
  g.widget = "files";
  return g;
}
const GsGroup kFilesGroup = filesGroup();
} // namespace
#endif

// Big-endian MAC order (byte 0 first), matching how the MAC is printed,
// so the id reads the same as the MAC address minus the colons.
String gwChipId() {
  uint64_t m = ESP.getEfuseMac();
  char buf[13];
  snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X", (uint8_t)m, (uint8_t)(m >> 8),
           (uint8_t)(m >> 16), (uint8_t)(m >> 24), (uint8_t)(m >> 32), (uint8_t)(m >> 40));
  return buf;
}

void gwGroupsRegister() {
#if defined(GARNET_WEB_WIFI)
  gwGroupsRegisterWifi();
#endif
#if defined(GARNET_WEB_ETH)
  gwGroupsRegisterEth();
#endif
#if defined(GARNET_WEB_BT)
  gwGroupsRegisterBt();
#endif
#if defined(GARNET_WEB_SD)
  gwGroupsRegisterSd();
#endif
#if defined(GARNET_WEB_FILES)
  gsRegister(kFilesGroup);
#endif
#if defined(GARNET_WEB_TIME)
  gwTimeRegister();
#endif
  gsRegister(kGroup);
}
