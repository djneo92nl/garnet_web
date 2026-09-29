#include "gw_internal.h"

#if defined(GARNET_WEB_LOG)

#include <esp_log.h>
#include <rom/ets_sys.h>

// Log group: the device's own log in the browser - no cable needed to see
// why a board misbehaves. Captures both paths output takes:
//   - Arduino log_x / log_printf -> ets_printf, via ets_install_putc2 (a
//     second character sink next to the UART one). Arduino resets it in
//     Serial.begin(), so gwLogLoop re-installs it - it's one pointer write.
//   - IDF ESP_LOGx -> esp_log_set_vprintf, teed to the original.
// Plain Serial.print() goes straight to the UART driver and isn't seen;
// use log_printf() for lines that should show up here too.
//
//   GET /api/log?since=N  -> {head, text}  (bytes after N, at most the ring)

namespace {

constexpr size_t kRingSize = 8192;

char ring[kRingSize];
uint32_t head = 0; // total bytes ever written; ring index = head % kRingSize
portMUX_TYPE ringLock = portMUX_INITIALIZER_UNLOCKED;
vprintf_like_t idfVprintf = nullptr;

// Called for every character, from any task and possibly an ISR - so a
// spinlock, no allocation, no blocking.
void IRAM_ATTR putChar(char c) {
  portENTER_CRITICAL_SAFE(&ringLock);
  ring[head % kRingSize] = c;
  head++;
  portEXIT_CRITICAL_SAFE(&ringLock);
}

int teeVprintf(const char *fmt, va_list args) {
  char buf[192];
  va_list copy;
  va_copy(copy, args);
  int n = vsnprintf(buf, sizeof(buf), fmt, copy);
  va_end(copy);
  for (int i = 0; i < n && i < (int)sizeof(buf) - 1; i++) putChar(buf[i]);
  return idfVprintf ? idfVprintf(fmt, args) : n;
}

String sizeStr() { return gwFmtBytes(kRingSize) + " ring buffer"; }

const GsField kFields[] = {gsInfo("buffer", "Keeps", sizeStr)};
const GsGroup kLogGroup = gsWithWidget(gsGroup("log", "Log", "file", kFields, "Tools"), "log");

} // namespace

void gwLogBegin() {
  gsRegister(kLogGroup);
  idfVprintf = esp_log_set_vprintf(teeVprintf);
  ets_install_putc2(putChar);
}

void gwLogLoop() { ets_install_putc2(putChar); }

bool gwLogHandle(httpd_req_t *req, const String &path, bool post, esp_err_t &err) {
  if (post || path != "/api/log") return false;
  uint32_t since = strtoul(gwHttpQuery(req, "since").c_str(), nullptr, 10);

  // Copy out under the lock, build JSON outside it.
  char *text = static_cast<char *>(malloc(kRingSize + 1));
  if (text == nullptr) {
    err = gwHttpError(req, "500 Internal Server Error", "out of memory");
    return true;
  }
  portENTER_CRITICAL(&ringLock);
  uint32_t end = head;
  // since > head: the device rebooted since the client last asked - resend all.
  if (since > end) since = 0;
  uint32_t start = end - since > kRingSize ? end - kRingSize : since;
  size_t n = 0;
  for (uint32_t i = start; i < end; i++) text[n++] = ring[i % kRingSize];
  portEXIT_CRITICAL(&ringLock);
  text[n] = '\0';

  cJSON *o = cJSON_CreateObject();
  cJSON_AddNumberToObject(o, "head", end);
  cJSON_AddBoolToObject(o, "gap", start > since); // older lines fell out of the ring
  cJSON_AddStringToObject(o, "text", text);
  free(text);
  err = gwHttpJson(req, o);
  return true;
}

#endif // GARNET_WEB_LOG
