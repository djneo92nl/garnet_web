#include "gw_internal.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

GwConfig gwCfg;

namespace {

enum class DeferKind : uint8_t { Changed, Action, Reboot };

struct DeferItem {
  DeferKind kind;
  const GsGroup *group;
  const GsField *field;
  uint32_t value; // Changed: mask, Reboot: delay ms
};

constexpr UBaseType_t kQueueLen = 8;

QueueHandle_t queue = nullptr;
SemaphoreHandle_t lock = nullptr;
uint32_t rebootAt = 0; // millis(), 0 = none

void push(const DeferItem &item) {
  // Short timeout: a full queue means gwLoop isn't being called, and the
  // server task must not hang forever on that.
  if (queue == nullptr || xQueueSend(queue, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
    log_e("garnet_web: deferred-work queue full - is gwLoop() being called?");
  }
}

} // namespace

void gwDeferInit() {
  if (queue == nullptr) queue = xQueueCreate(kQueueLen, sizeof(DeferItem));
  if (lock == nullptr) lock = xSemaphoreCreateMutex();
}

void gwDeferChanged(const GsGroup *group, uint32_t mask) {
  if (mask != 0) push({DeferKind::Changed, group, nullptr, mask});
}

void gwDeferAction(const GsField *field) { push({DeferKind::Action, nullptr, field, 0}); }

void gwDeferReboot(uint32_t delayMs) { push({DeferKind::Reboot, nullptr, nullptr, delayMs}); }

void gwDeferLoop() {
  DeferItem item;
  while (queue != nullptr && xQueueReceive(queue, &item, 0) == pdTRUE) {
    switch (item.kind) {
    case DeferKind::Changed: gsNotifyChanged(*item.group, item.value); break;
    case DeferKind::Action:
      if (item.field->action) item.field->action();
      break;
    case DeferKind::Reboot:
      // Delay so the HTTP response ("rebooting...") actually leaves the
      // socket before the radio goes down.
      rebootAt = millis() + (item.value ? item.value : 1);
      break;
    }
  }
  if (rebootAt != 0 && (int32_t)(millis() - rebootAt) >= 0) {
    log_i("garnet_web: rebooting to apply settings");
    delay(50);
    ESP.restart();
  }
}

void gwLock() {
  if (lock) xSemaphoreTake(lock, portMAX_DELAY);
}

void gwUnlock() {
  if (lock) xSemaphoreGive(lock);
}

void gwBegin(const GwConfig &cfg) {
  gwCfg = cfg;
  if (gwCfg.defaultPassword == nullptr || gwCfg.defaultPassword[0] == '\0') {
    // "Always password" is the contract - refuse to run open instead of
    // quietly shipping a device anyone on the LAN can reconfigure.
    log_e("garnet_web: GwConfig.defaultPassword is required - web UI disabled");
  }
  gwDeferInit();
  gwGroupsRegister();
  gwAuthBegin();
#if defined(GARNET_WEB_BT)
  gwBtBegin();
#endif
  gwNetBegin();
  if (gwCfg.defaultPassword != nullptr && gwCfg.defaultPassword[0] != '\0') gwServerBegin();
}

void gwLoop() {
  gwNetLoop();
#if defined(GARNET_WEB_SD)
  gwSdLoop();
#endif
  gwDeferLoop();
}

// ---- Formatting helpers -----------------------------------------------------------

String gwFmtBytes(uint64_t bytes) {
  char buf[24];
  if (bytes >= 1024ULL * 1024 * 1024) {
    snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0 * 1024 * 1024));
  } else if (bytes >= 1024 * 1024) {
    snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024));
  } else if (bytes >= 1024) {
    snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
  } else {
    snprintf(buf, sizeof(buf), "%u B", (unsigned)bytes);
  }
  return buf;
}

String gwFmtUptime(uint32_t s) {
  char buf[32];
  uint32_t d = s / 86400, h = (s / 3600) % 24, m = (s / 60) % 60;
  if (d) snprintf(buf, sizeof(buf), "%ud %uh %um", (unsigned)d, (unsigned)h, (unsigned)m);
  else if (h) snprintf(buf, sizeof(buf), "%uh %um", (unsigned)h, (unsigned)m);
  else snprintf(buf, sizeof(buf), "%um %us", (unsigned)m, (unsigned)(s % 60));
  return buf;
}
