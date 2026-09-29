#include "../gw_internal.h"

#include <soc/soc_caps.h>

// Bluetooth and SD card groups.

// ---- Bluetooth ----------------------------------------------------------------------
// On/off + advertised name, applied once at boot (the group reboots on
// save - tearing Bluedroid down and up again at runtime is exactly the kind
// of thing that leaks memory). Chips without a BT radio of their own (the
// ESP32-P4, S2) compile the group out rather than failing to build.

#if defined(GARNET_WEB_BT) && SOC_BT_SUPPORTED
#include <BLEDevice.h>

namespace {
constexpr const char *kBtGroup = "bt";
bool btRunning = false;

String btStatusStr() { return btRunning ? "Advertising" : "Off"; }

const GsField kBtFields[] = {
    gsInfo("status", "Status", btStatusStr),
    gsToggle("enabled", "Bluetooth", false),
    gsWithShowIf(gsWithPlaceholder(gsText("name", "Device name", "", 29), "same as hostname"),
                 "enabled"),
};

const GsGroup kBt =
    gsWithReboot(gsGroup(kBtGroup, "Bluetooth", "bluetooth", kBtFields, "Connections"));
} // namespace

void gwGroupsRegisterBt() { gsRegister(kBt); }

void gwBtBegin() {
  if (!gsGetBool(kBtGroup, "enabled")) {
    // Hand the BT controller's RAM (~30 KB) back to the heap - nobody will
    // use Bluetooth this boot.
#if CONFIG_IDF_TARGET_ESP32
    esp_bt_controller_mem_release(ESP_BT_MODE_BTDM); // classic + BLE controller
#else
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE); // S3 / C3 / C6: BLE only
#endif
    return;
  }
  String name = gsGetString(kBtGroup, "name");
  if (name.length() == 0) name = gwHostname().length() ? gwHostname() : String(gwCfg.name);
  BLEDevice::init(name.c_str());
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  btRunning = true;
}
#elif defined(GARNET_WEB_BT)
void gwGroupsRegisterBt() {}
void gwBtBegin() {}
#endif

// ---- SD card -------------------------------------------------------------------------
// Info only. Card size/usage is sampled on gwLoop's task (the app's
// task, which owns the SPI bus) and cached: FAT free-space counting can
// take seconds on a big card, and must never run on the server task
// in the middle of an app's own SD transfer.

#if defined(GARNET_WEB_SD)
// Included here, not in garnet_web.h (see the forward declarations there).
#include <SD.h>
#if SOC_SDMMC_HOST_SUPPORTED
#include <SD_MMC.h>
#endif

namespace {
constexpr uint32_t kSdRefreshMs = 30000;

// SD (SPI) and SD_MMC are unrelated classes with the same info methods;
// keep whichever the app gave us.
fs::SDFS *sd = nullptr;
#if SOC_SDMMC_HOST_SUPPORTED
fs::SDMMCFS *sdmmc = nullptr;
#endif
struct SdCache {
  bool mounted = false;
  uint8_t type = CARD_NONE;
  uint64_t cardSize = 0, total = 0, used = 0;
} cache; // guarded by gwLock
uint32_t lastRefresh = 0;
bool refreshedOnce = false;

SdCache snapshot() {
  gwLock();
  SdCache c = cache;
  gwUnlock();
  return c;
}

String sdStatusStr() { return snapshot().mounted ? "Mounted" : "Not mounted"; }
String sdTypeStr() {
  switch (snapshot().type) {
  case CARD_MMC: return "MMC";
  case CARD_SD: return "SD";
  case CARD_SDHC: return "SDHC / SDXC";
  case CARD_NONE: return "-";
  default: return "Unknown";
  }
}
String sdSizeStr() {
  SdCache c = snapshot();
  return c.mounted ? gwFmtBytes(c.cardSize) : String("-");
}
String sdUsedStr() {
  SdCache c = snapshot();
  if (!c.mounted || c.total == 0) return "-";
  return gwFmtBytes(c.used) + " of " + gwFmtBytes(c.total) + " (" +
         String((unsigned)(c.used * 100 / c.total)) + "%)";
}
String sdFreeStr() {
  SdCache c = snapshot();
  return c.mounted ? gwFmtBytes(c.total - c.used) : String("-");
}

const GsField kSdFields[] = {
    gsInfo("status", "Status", sdStatusStr), gsInfo("type", "Card type", sdTypeStr),
    gsInfo("size", "Capacity", sdSizeStr),   gsInfo("used", "Used", sdUsedStr),
    gsInfo("free", "Free", sdFreeStr),
};

const GsGroup kSd = gsGroup("sd", "SD Card", "sdcard", kSdFields, "Device");
} // namespace

void gwGroupsRegisterSd() { gsRegister(kSd); }

void gwSetSd(fs::SDFS &card) {
  sd = &card;
  refreshedOnce = false; // sample on the next gwLoop
}

#if SOC_SDMMC_HOST_SUPPORTED
void gwSetSd(fs::SDMMCFS &card) {
  sdmmc = &card;
  refreshedOnce = false;
}
#endif

namespace {
template <typename Card> SdCache sample(Card &card) {
  SdCache c;
  c.type = card.cardType();
  c.mounted = c.type != CARD_NONE;
  if (c.mounted) {
    c.cardSize = card.cardSize();
    c.total = card.totalBytes();
    c.used = card.usedBytes();
  }
  return c;
}
} // namespace

void gwSdLoop() {
  bool have = sd != nullptr;
#if SOC_SDMMC_HOST_SUPPORTED
  have = have || sdmmc != nullptr;
#endif
  if (!have) return;
  if (refreshedOnce && millis() - lastRefresh < kSdRefreshMs) return;
  refreshedOnce = true;
  lastRefresh = millis();
  SdCache c;
  if (sd) c = sample(*sd);
#if SOC_SDMMC_HOST_SUPPORTED
  else c = sample(*sdmmc);
#endif
  gwLock();
  cache = c;
  gwUnlock();
}
#endif
