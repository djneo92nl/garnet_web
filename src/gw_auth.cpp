#include "gw_internal.h"

#include <Preferences.h>
#include <esp_random.h>
#include <mbedtls/sha256.h>

// Web UI login. The password is stored in NVS only as a salted SHA-256.
// Until someone changes it in the UI, nothing is stored and the code's
// GwConfig.defaultPassword applies, so changing the default in firmware
// still takes effect on devices whose password was never changed.
//
// Sessions are random tokens held in RAM (a reboot logs everyone out),
// sent as an HttpOnly SameSite=Strict cookie. POSTs additionally need an
// "X-GW: 1" header (checked in gw_server) - a cross-site form can't set that.

namespace {

constexpr const char *kNs = "gw_auth";
constexpr size_t kSaltLen = 16;
constexpr uint8_t kMaxSessions = 4;
constexpr uint32_t kSessionIdleMs = 12UL * 60 * 60 * 1000;
constexpr uint8_t kMaxFailures = 5;
constexpr uint32_t kLockoutMs = 30000;
constexpr size_t kMinPasswordLen = 6;

struct Session {
  char token[kGwTokenLen + 1] = {};
  uint32_t lastUsed = 0;
};

Session sessions[kMaxSessions];
uint8_t failures = 0;
uint32_t lockedUntil = 0;

void hash(const uint8_t *salt, const String &password, uint8_t out[32]) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, salt, kSaltLen);
  mbedtls_sha256_update(&ctx, reinterpret_cast<const uint8_t *>(password.c_str()),
                        password.length());
  mbedtls_sha256_finish(&ctx, out);
  mbedtls_sha256_free(&ctx);
}

// Constant-time compare - timing leaks are cheap to avoid here.
bool sameBytes(const uint8_t *a, const uint8_t *b, size_t n) {
  uint8_t diff = 0;
  for (size_t i = 0; i < n; i++) diff |= a[i] ^ b[i];
  return diff == 0;
}

bool passwordMatches(const String &password) {
  Preferences p;
  uint8_t salt[kSaltLen];
  uint8_t stored[32];
  bool haveStored = false;
  if (gsNamespaceExists(kNs) && p.begin(kNs, true)) {
    haveStored = p.getBytes("salt", salt, kSaltLen) == kSaltLen &&
                 p.getBytes("hash", stored, 32) == 32;
    p.end();
  }
  if (!haveStored) {
    const char *def = gwCfg.defaultPassword ? gwCfg.defaultPassword : "";
    size_t n = strlen(def);
    return n > 0 && n == password.length() &&
           sameBytes(reinterpret_cast<const uint8_t *>(def),
                     reinterpret_cast<const uint8_t *>(password.c_str()), n);
  }
  uint8_t candidate[32];
  hash(salt, password, candidate);
  return sameBytes(candidate, stored, 32);
}

void newToken(char out[kGwTokenLen + 1]) {
  static const char kHex[] = "0123456789abcdef";
  uint8_t raw[kGwTokenLen / 2];
  // esp_fill_random is a true RNG while the radio is on (always the case
  // for a web UI), which is what makes these tokens unguessable.
  esp_fill_random(raw, sizeof(raw));
  for (size_t i = 0; i < sizeof(raw); i++) {
    out[i * 2] = kHex[raw[i] >> 4];
    out[i * 2 + 1] = kHex[raw[i] & 0x0f];
  }
  out[kGwTokenLen] = '\0';
}

bool readCookie(httpd_req_t *req, char out[kGwTokenLen + 1]) {
  size_t len = kGwTokenLen + 1;
  if (httpd_req_get_cookie_val(req, "gw_s", out, &len) != ESP_OK) return false;
  return strlen(out) == kGwTokenLen;
}

int findSession(const char *token) {
  uint32_t now = millis();
  for (uint8_t i = 0; i < kMaxSessions; i++) {
    Session &s = sessions[i];
    if (s.token[0] == '\0') continue;
    if (now - s.lastUsed > kSessionIdleMs) {
      s.token[0] = '\0';
      continue;
    }
    if (sameBytes(reinterpret_cast<const uint8_t *>(s.token),
                  reinterpret_cast<const uint8_t *>(token), kGwTokenLen)) {
      return i;
    }
  }
  return -1;
}

} // namespace

void gwAuthBegin() {}

bool gwAuthCheck(httpd_req_t *req) {
  char token[kGwTokenLen + 1];
  if (!readCookie(req, token)) return false;
  gwLock();
  int i = findSession(token);
  if (i >= 0) sessions[i].lastUsed = millis();
  gwUnlock();
  return i >= 0;
}

bool gwAuthorized(httpd_req_t *req) { return gwAuthCheck(req); }

bool gwAuthLogin(const String &password, String &tokenOut, uint32_t &retryAfterS) {
  retryAfterS = 0;
  uint32_t now = millis();
  if (lockedUntil != 0 && (int32_t)(lockedUntil - now) > 0) {
    retryAfterS = (lockedUntil - now + 999) / 1000;
    return false;
  }
  if (!passwordMatches(password)) {
    // Global (not per-client) lockout: the device can't reliably tell
    // clients apart behind NAT, and a settings page sees ~no traffic, so
    // slowing everyone down during an attack costs nothing real.
    if (++failures >= kMaxFailures) {
      failures = 0;
      lockedUntil = now + kLockoutMs;
      retryAfterS = kLockoutMs / 1000;
    }
    return false;
  }
  failures = 0;
  lockedUntil = 0;

  gwLock();
  // Reuse an empty slot, else evict the least recently used session.
  uint8_t slot = 0;
  for (uint8_t i = 0; i < kMaxSessions; i++) {
    if (sessions[i].token[0] == '\0') { slot = i; break; }
    if (sessions[i].lastUsed < sessions[slot].lastUsed) slot = i;
  }
  newToken(sessions[slot].token);
  sessions[slot].lastUsed = now;
  tokenOut = sessions[slot].token;
  gwUnlock();
  return true;
}

void gwAuthLogout(httpd_req_t *req) {
  char token[kGwTokenLen + 1];
  if (!readCookie(req, token)) return;
  gwLock();
  int i = findSession(token);
  if (i >= 0) sessions[i].token[0] = '\0';
  gwUnlock();
}

void gwAuthEndAllSessions() {
  gwLock();
  for (Session &s : sessions) s.token[0] = '\0';
  gwUnlock();
}

bool gwAuthResetPassword() {
  // No stored hash = GwConfig.defaultPassword applies again (see top).
  bool ok = true;
  if (gsNamespaceExists(kNs)) {
    Preferences p;
    ok = p.begin(kNs, false) && p.clear();
    p.end();
  }
  failures = 0;
  lockedUntil = 0;
  gwAuthEndAllSessions();
  return ok;
}

bool gwAuthChangePassword(const String &current, const String &next, String &why) {
  if (!passwordMatches(current)) {
    why = "Current password is wrong";
    return false;
  }
  return gwAuthSetPassword(next, why);
}

bool gwAuthSetPassword(const String &next, String &why) {
  if (next.length() < kMinPasswordLen) {
    why = "Use at least " + String(kMinPasswordLen) + " characters";
    return false;
  }
  if (next.length() > 64) {
    why = "Use at most 64 characters";
    return false;
  }
  uint8_t salt[kSaltLen];
  uint8_t h[32];
  esp_fill_random(salt, kSaltLen);
  hash(salt, next, h);
  Preferences p;
  if (!p.begin(kNs, false)) {
    why = "Storage unavailable";
    return false;
  }
  bool ok = p.putBytes("salt", salt, kSaltLen) == kSaltLen && p.putBytes("hash", h, 32) == 32;
  p.end();
  if (!ok) why = "Could not store password";
  return ok;
}
