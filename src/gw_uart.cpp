#include "gw_internal.h"

#if defined(GARNET_WEB_UART)

#include <driver/gpio.h>
#include <esp_private/esp_gpio_reserve.h>

// Serial group: a UART monitor in the browser - Serial1 on any two pins,
// to watch (and talk to) whatever the board gets wired to: a GPS, a
// modem, another MCU's console. The console UART stays untouched.
//
// Pins/baud are settings (applied live by onSave, on the loop task); RX
// bytes are pumped into a ring by gwUartLoop, TX goes through gwRunOnLoop,
// so Serial1 is only ever touched from the loop task.
//
//   GET  /api/uart?since=N          -> {head, gap, hex, open, canSend}
//   POST /api/uart/send {text, eol} -> writes text + "", \n, \r or \r\n

namespace {

constexpr const char *kGroup = "serial";
constexpr size_t kRingSize = 4096;

const char *const kBauds[] = {"1200",  "2400",   "4800",   "9600",   "19200",  "38400",
                              "57600", "74880",  "115200", "230400", "460800", "921600"};
const uint32_t kBaudValues[] = {1200,  2400,  4800,   9600,   19200,  38400,
                                57600, 74880, 115200, 230400, 460800, 921600};
constexpr uint8_t kBaudCount = sizeof(kBauds) / sizeof(kBauds[0]);

uint8_t ring[kRingSize];
uint32_t head = 0;
portMUX_TYPE ringLock = portMUX_INITIALIZER_UNLOCKED;

bool uartOpen = false;
int openRx = -1, openTx = -1;
String status = "Off";

bool pinOk(int pin) {
  return GPIO_IS_VALID_GPIO(pin) && !esp_gpio_is_reserved(BIT64(pin)) && pin != TX && pin != RX;
}

void apply() {
  if (uartOpen) {
    Serial1.end();
    uartOpen = false;
    openRx = openTx = -1;
  }
  if (!gsGetBool(kGroup, "enabled")) {
    status = "Off";
    return;
  }
  int rx = gsGetInt(kGroup, "rx"), tx = gsGetInt(kGroup, "tx");
  int b = gsGetInt(kGroup, "baud");
  uint32_t baud = kBaudValues[b >= 0 && b < kBaudCount ? b : 8];
  // TX is optional (-1 = listen only), RX is not.
  if (!pinOk(rx) || (tx != -1 && (!pinOk(tx) || !GPIO_IS_VALID_OUTPUT_GPIO(tx))) || rx == tx) {
    status = "Invalid pins (reserved, console, or RX = TX)";
    return;
  }
  Serial1.setRxBufferSize(1024);
  Serial1.begin(baud, SERIAL_8N1, rx, tx);
  uartOpen = true;
  openRx = rx;
  openTx = tx;
  status = "Open, " + String(baud) + " baud, RX " + String(rx) +
           (tx >= 0 ? ", TX " + String(tx) : String(", listen only"));
}

void onSaved(const GsGroup &) { apply(); }

String statusStr() { return status; }

const GsField kFields[] = {
    gsInfo("status", "Status", statusStr),
    gsToggle("enabled", "Enabled", false),
    gsWithShowIf(gsNumber("rx", "RX pin", -1, -1, GPIO_NUM_MAX - 1), "enabled"),
    gsWithShowIf(gsWithHelp(gsNumber("tx", "TX pin", -1, -1, GPIO_NUM_MAX - 1), "-1 = listen only"),
                 "enabled"),
    gsWithShowIf(gsSelect("baud", "Baud", kBauds, kBaudCount, 8), "enabled"),
};

const GsGroup kSerialGroup =
    gsWithWidget(gsWithOnSave(gsGroup(kGroup, "Serial", "sensor", kFields, "Tools"), onSaved), "uart");

struct SendJob {
  uint16_t len;
  uint8_t data[260];
  bool sent;
};

void runSend(void *p) {
  SendJob &j = *static_cast<SendJob *>(p);
  j.sent = uartOpen && openTx >= 0;
  if (j.sent) Serial1.write(j.data, j.len);
}

} // namespace

bool gwUartUsesPin(int pin) { return uartOpen && (pin == openRx || pin == openTx); }

void gwUartBegin() {
  gsRegister(kSerialGroup);
  apply();
}

void gwUartLoop() {
  if (!uartOpen) return;
  uint8_t buf[128];
  int n;
  while ((n = Serial1.available()) > 0) {
    n = Serial1.read(buf, min((size_t)n, sizeof(buf)));
    portENTER_CRITICAL(&ringLock);
    for (int i = 0; i < n; i++) ring[(head + i) % kRingSize] = buf[i];
    head += n;
    portEXIT_CRITICAL(&ringLock);
  }
}

bool gwUartHandle(httpd_req_t *req, const String &path, bool post, esp_err_t &err) {
  if (!path.startsWith("/api/uart")) return false;

  if (!post && path == "/api/uart") {
    uint32_t since = strtoul(gwHttpQuery(req, "since").c_str(), nullptr, 10);
    // Bytes as hex: the stream is arbitrary binary, which a JSON string
    // can't carry. The UI renders it as text or a hex dump.
    char *hex = static_cast<char *>(malloc(kRingSize * 2 + 1));
    if (hex == nullptr) {
      err = gwHttpError(req, "500 Internal Server Error", "out of memory");
      return true;
    }
    static const char kHex[] = "0123456789abcdef";
    portENTER_CRITICAL(&ringLock);
    uint32_t end = head;
    if (since > end) since = 0; // rebooted since the client last asked
    uint32_t start = end - since > kRingSize ? end - kRingSize : since;
    size_t n = 0;
    for (uint32_t i = start; i < end; i++) {
      uint8_t b = ring[i % kRingSize];
      hex[n++] = kHex[b >> 4];
      hex[n++] = kHex[b & 15];
    }
    portEXIT_CRITICAL(&ringLock);
    hex[n] = '\0';
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "head", end);
    cJSON_AddBoolToObject(o, "gap", start > since);
    cJSON_AddBoolToObject(o, "open", uartOpen);
    cJSON_AddBoolToObject(o, "canSend", uartOpen && openTx >= 0);
    cJSON_AddStringToObject(o, "hex", hex);
    free(hex);
    err = gwHttpJson(req, o);
    return true;
  }

  if (post && path == "/api/uart/send") {
    cJSON *body = gwHttpReadJson(req);
    if (body == nullptr) {
      err = ESP_OK;
      return true;
    }
    String text = gwHttpJsonStr(body, "text");
    String eol = gwHttpJsonStr(body, "eol");
    cJSON_Delete(body);
    if (eol == "lf") text += "\n";
    else if (eol == "cr") text += "\r";
    else if (eol == "crlf") text += "\r\n";
    SendJob j = {};
    if (text.length() == 0 || text.length() > sizeof(j.data)) {
      err = gwHttpError(req, "422 Unprocessable Entity", "Send 1-256 characters");
      return true;
    }
    j.len = text.length();
    memcpy(j.data, text.c_str(), j.len);
    if (!gwRunOnLoop(runSend, &j, sizeof(j), 3000)) {
      err = gwHttpError(req, "503 Service Unavailable", "Device busy");
    } else if (!j.sent) {
      err = gwHttpError(req, "422 Unprocessable Entity", "Serial is off or listen-only");
    } else {
      err = gwHttpOk(req);
    }
    return true;
  }
  return false;
}

#endif // GARNET_WEB_UART
