#include "gw_internal.h"

#if defined(GARNET_WEB_HW)

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_private/esp_gpio_reserve.h>

// Hardware group: "what is this board wired to?" from a browser.
//   I2C scan on any two pins (with a stuck-bus check first)
//   GPIO tester: read (floating / pull-up / pull-down), drive high/low, ADC mV
//   input sweep: every usable pin as input, one read each
// All hardware access runs on gwLoop's task via gwRunOnLoop, never on the
// server task - the app owns the hardware from its loop. Nothing is
// touched until asked.
//
// The scan uses the IDF I2C master driver (i2c_master_probe), not Wire:
// PlatformIO's dependency scan follows #include <Wire.h> even behind an
// #if, which linked Wire into every garnet_web build (~27 KB) whether this
// feature was on or not. The IDF driver is precompiled and only linked
// when used - and it picks a free I2C port, so an app's own Wire is safe.
//
// Guarded pins: flash / PSRAM pins (esp_gpio_is_reserved - touching those
// crashes the chip) are refused outright; the console UART pins can be
// read but not driven (that would cut off the serial log).
//
//   GET  /api/hw                        pins + defaults
//   POST /api/hw/i2c    {sda,scl}       -> {found:[addr...]}
//   POST /api/hw/gpio   {pin,op}        op: read|pullup|pulldown|high|low|adc
//   POST /api/hw/inputs                 -> {pins:[{n,level}]}

namespace {

constexpr uint32_t kJobTimeoutMs = 6000;

bool reserved(int pin) { return esp_gpio_is_reserved(BIT64(pin)); }
bool console(int pin) {
#if defined(GARNET_WEB_UART)
  if (gwUartUsesPin(pin)) return true; // the serial monitor's pins count as console too
#endif
  return pin == TX || pin == RX;
}
bool usable(int pin) { return GPIO_IS_VALID_GPIO(pin) && !reserved(pin); }

// ---- Jobs (run on the loop task) ------------------------------------------------------

struct I2cJob {
  int sda, scl;
  bool stuck;
  bool ok;
  uint8_t count;
  uint8_t found[112]; // 0x08..0x77
};

void runI2c(void *p) {
  I2cJob &j = *static_cast<I2cJob *>(p);
  // Idle I2C lines sit high. Low with the internal pull-up on = shorted,
  // a device holding the bus, or the wrong pins - scanning would only
  // produce 120 timeouts.
  pinMode(j.sda, INPUT_PULLUP);
  pinMode(j.scl, INPUT_PULLUP);
  delay(2);
  j.stuck = digitalRead(j.sda) == LOW || digitalRead(j.scl) == LOW;
  pinMode(j.sda, INPUT);
  pinMode(j.scl, INPUT);
  if (j.stuck) return;
  i2c_master_bus_config_t cfg = {};
  cfg.i2c_port = -1; // any free port
  cfg.sda_io_num = (gpio_num_t)j.sda;
  cfg.scl_io_num = (gpio_num_t)j.scl;
  cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  cfg.glitch_ignore_cnt = 7;
  cfg.flags.enable_internal_pullup = true;
  i2c_master_bus_handle_t bus;
  if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) return;
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    if (i2c_master_probe(bus, a, 20) == ESP_OK) j.found[j.count++] = a;
  }
  i2c_del_master_bus(bus);
  // Hand the pins back as plain inputs, the state the board booted in.
  pinMode(j.sda, INPUT);
  pinMode(j.scl, INPUT);
  j.ok = true;
}

struct GpioJob {
  int pin;
  char op[10];
  int level; // -1 = n/a
  int mv;    // -1 = n/a
};

void runGpio(void *p) {
  GpioJob &j = *static_cast<GpioJob *>(p);
  j.level = -1;
  j.mv = -1;
  String op = j.op;
  if (op == "read" || op == "pullup" || op == "pulldown") {
    pinMode(j.pin, op == "pullup" ? INPUT_PULLUP : op == "pulldown" ? INPUT_PULLDOWN : INPUT);
    delay(1);
    j.level = digitalRead(j.pin);
  } else if (op == "high" || op == "low") {
    pinMode(j.pin, OUTPUT);
    digitalWrite(j.pin, op == "high" ? HIGH : LOW);
    j.level = op == "high" ? 1 : 0;
  } else if (op == "adc") {
    j.mv = analogReadMilliVolts(j.pin);
  }
}

struct InputsJob {
  uint8_t count;
  uint8_t pins[64];
  uint8_t levels[64];
};

void runInputs(void *p) {
  InputsJob &j = *static_cast<InputsJob *>(p);
  for (int pin = 0; pin < GPIO_NUM_MAX && j.count < 64; pin++) {
    if (!usable(pin) || console(pin)) continue;
    pinMode(pin, INPUT);
    j.pins[j.count] = pin;
    j.levels[j.count] = digitalRead(pin);
    j.count++;
  }
}

// ---- Handlers -----------------------------------------------------------------------------

esp_err_t info(httpd_req_t *req) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddNumberToObject(o, "sda", SDA);
  cJSON_AddNumberToObject(o, "scl", SCL);
  cJSON_AddNumberToObject(o, "tx", TX);
  cJSON_AddNumberToObject(o, "rx", RX);
  cJSON *pins = cJSON_AddArrayToObject(o, "pins");
  for (int pin = 0; pin < GPIO_NUM_MAX; pin++) {
    if (!GPIO_IS_VALID_GPIO(pin)) continue;
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "n", pin);
    if (reserved(pin)) cJSON_AddBoolToObject(p, "reserved", true);
    if (console(pin)) cJSON_AddBoolToObject(p, "console", true);
    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) cJSON_AddBoolToObject(p, "inputOnly", true);
    if (digitalPinToAnalogChannel(pin) >= 0) cJSON_AddBoolToObject(p, "adc", true);
    cJSON_AddItemToArray(pins, p);
  }
  return gwHttpJson(req, o);
}

esp_err_t i2c(httpd_req_t *req, const cJSON *body) {
  I2cJob j = {};
  j.sda = gwHttpJsonInt(body, "sda", SDA);
  j.scl = gwHttpJsonInt(body, "scl", SCL);
  if (!usable(j.sda) || !usable(j.scl) || j.sda == j.scl || !GPIO_IS_VALID_OUTPUT_GPIO(j.sda) ||
      !GPIO_IS_VALID_OUTPUT_GPIO(j.scl)) {
    return gwHttpError(req, "422 Unprocessable Entity", "SDA / SCL must be two different usable output pins");
  }
  if (console(j.sda) || console(j.scl)) {
    return gwHttpError(req, "422 Unprocessable Entity", "Pin is the serial console");
  }
  if (!gwRunOnLoop(runI2c, &j, sizeof(j), kJobTimeoutMs)) {
    return gwHttpError(req, "503 Service Unavailable", "Device busy");
  }
  if (j.stuck) {
    return gwHttpError(req, "422 Unprocessable Entity",
                       "SDA or SCL is held low (wrong pins, short, or a stuck device)");
  }
  if (!j.ok) return gwHttpError(req, "500 Internal Server Error", "I2C init failed");
  cJSON *o = cJSON_CreateObject();
  cJSON *found = cJSON_AddArrayToObject(o, "found");
  for (uint8_t i = 0; i < j.count; i++) cJSON_AddItemToArray(found, cJSON_CreateNumber(j.found[i]));
  return gwHttpJson(req, o);
}

esp_err_t gpio(httpd_req_t *req, const cJSON *body) {
  GpioJob j = {};
  j.pin = gwHttpJsonInt(body, "pin", -1);
  String op = gwHttpJsonStr(body, "op");
  if (!GPIO_IS_VALID_GPIO(j.pin)) return gwHttpError(req, "422 Unprocessable Entity", "No such GPIO");
  if (reserved(j.pin)) {
    return gwHttpError(req, "422 Unprocessable Entity", "Reserved for flash / PSRAM - not safe to touch");
  }
  bool drive = op == "high" || op == "low";
  if (!drive && op != "read" && op != "pullup" && op != "pulldown" && op != "adc") {
    return gwHttpError(req, "422 Unprocessable Entity", "Unknown operation");
  }
  if (drive && console(j.pin)) {
    return gwHttpError(req, "422 Unprocessable Entity", "Serial console pin - read only");
  }
  if (drive && !GPIO_IS_VALID_OUTPUT_GPIO(j.pin)) {
    return gwHttpError(req, "422 Unprocessable Entity", "Input-only pin");
  }
  if (op == "adc" && digitalPinToAnalogChannel(j.pin) < 0) {
    return gwHttpError(req, "422 Unprocessable Entity", "Not an ADC pin");
  }
  strlcpy(j.op, op.c_str(), sizeof(j.op));
  if (!gwRunOnLoop(runGpio, &j, sizeof(j), kJobTimeoutMs)) {
    return gwHttpError(req, "503 Service Unavailable", "Device busy");
  }
  cJSON *o = cJSON_CreateObject();
  cJSON_AddNumberToObject(o, "pin", j.pin);
  if (j.level >= 0) cJSON_AddNumberToObject(o, "level", j.level);
  if (j.mv >= 0) cJSON_AddNumberToObject(o, "mv", j.mv);
  return gwHttpJson(req, o);
}

esp_err_t inputs(httpd_req_t *req) {
  InputsJob j = {};
  if (!gwRunOnLoop(runInputs, &j, sizeof(j), kJobTimeoutMs)) {
    return gwHttpError(req, "503 Service Unavailable", "Device busy");
  }
  cJSON *o = cJSON_CreateObject();
  cJSON *pins = cJSON_AddArrayToObject(o, "pins");
  for (uint8_t i = 0; i < j.count; i++) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "n", j.pins[i]);
    cJSON_AddNumberToObject(p, "level", j.levels[i]);
    cJSON_AddItemToArray(pins, p);
  }
  return gwHttpJson(req, o);
}

GsGroup hwGroup() {
  GsGroup g{};
  g.id = "hw";
  g.title = "Hardware";
  g.icon = "system";
  g.section = "Tools";
  g.widget = "hw";
  return g;
}
const GsGroup kHwGroup = hwGroup();

} // namespace

void gwHwBegin() { gsRegister(kHwGroup); }

bool gwHwHandle(httpd_req_t *req, const String &path, bool post, esp_err_t &err) {
  if (!path.startsWith("/api/hw")) return false;
  if (!post && path == "/api/hw") {
    err = info(req);
    return true;
  }
  if (post && path == "/api/hw/inputs") {
    err = inputs(req);
    return true;
  }
  if (post && (path == "/api/hw/i2c" || path == "/api/hw/gpio")) {
    cJSON *body = gwHttpReadJson(req);
    if (body == nullptr) {
      err = ESP_OK;
      return true;
    }
    err = path == "/api/hw/i2c" ? i2c(req, body) : gpio(req, body);
    cJSON_Delete(body);
    return true;
  }
  return false;
}

#endif // GARNET_WEB_HW
