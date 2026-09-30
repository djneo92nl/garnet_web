#include "gw_internal.h"

#if defined(GARNET_WEB_SERIAL)

// Serial console commands: provision a board over USB without the setup AP
// - handy for a whole drawer of boards, or from a script:
//
//   gw wifi "My Network" "secret"   save a network, restart, join it
//   gw wifi "Open Cafe"             open network (no password)
//   gw forget "My Network"          remove a saved network
//   gw password reset               web password back to the firmware default
//   gw password "new-secret"        set a new web password
//   gw status                       network, address, hostname, chip ID
//   gw help
//
// Arguments are space-separated; quote those with spaces, \" and \\ escape.
// Replies are one line starting "OK" or "ERR" (plus info lines for
// status), so a script can wait for them. No web password: whoever is on
// the serial port has physical access and could reflash the board anyway.
//
// Runs from gwLoop (the app's task), reading whatever Serial is - the UART
// console or USB CDC. Only lines starting with "gw " are taken; anything
// else is ignored, so a terminal's stray input does no harm.

namespace {

constexpr size_t kMaxLine = 200;
constexpr uint8_t kMaxArgs = 4;

char line[kMaxLine + 1];
size_t lineLen = 0;
bool overflow = false;

// Splits `s` into args, honouring "quotes" and \ escapes. Returns the count,
// or -1 on an unterminated quote.
int splitArgs(const char *s, String args[], uint8_t max) {
  int n = 0;
  while (*s) {
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) break;
    if (n == max) return n; // extra words are ignored
    String arg;
    bool quoted = *s == '"';
    if (quoted) s++;
    while (*s && (quoted ? *s != '"' : (*s != ' ' && *s != '\t'))) {
      if (*s == '\\' && (s[1] == '"' || s[1] == '\\')) s++;
      arg += *s++;
    }
    if (quoted) {
      if (*s != '"') return -1;
      s++;
    }
    args[n++] = arg;
  }
  return n;
}

const char *modeName(GwNetMode m) {
  switch (m) {
  case GwNetMode::Ethernet: return "ethernet";
  case GwNetMode::WiFi: return "wifi";
  case GwNetMode::Portal: return "setup-ap";
  default: return "offline";
  }
}

void status() {
  Serial.printf("OK %s ip=%s host=%s.local id=%s\n", modeName(gwNetMode()),
                gwNetIP().toString().c_str(), gwHostname().c_str(), gwChipId().c_str());
#if defined(GARNET_WEB_WIFI)
  String ssids[kGwMaxSavedNets];
  uint8_t n = gwWifiSavedSsids(ssids, kGwMaxSavedNets);
  for (uint8_t i = 0; i < n; i++) Serial.printf("  saved: \"%s\"\n", ssids[i].c_str());
  if (n == 0) Serial.println("  saved: none");
#endif
}

void help() {
  Serial.println("OK commands:");
#if defined(GARNET_WEB_WIFI)
  Serial.println("  gw wifi \"<ssid>\" \"<password>\"   save network, restart, join it");
  Serial.println("  gw wifi \"<ssid>\"                open network");
  Serial.println("  gw forget \"<ssid>\"              remove a saved network");
#endif
  Serial.println("  gw password reset               web password back to the firmware default");
  Serial.println("  gw password \"<new>\"             set a new web password (6-64 chars)");
  Serial.println("  gw status                       network, address, hostname, chip id");
}

void run(const char *text) {
  String args[kMaxArgs];
  int n = splitArgs(text, args, kMaxArgs);
  if (n < 0) {
    Serial.println("ERR unterminated quote");
    return;
  }
  if (n < 2 || args[0] != "gw") return; // not for us
  const String &cmd = args[1];

  if (cmd == "status") return status();
  if (cmd == "help") return help();
  if (cmd == "password") {
    // The recovery path for a forgotten web password: without it, that
    // meant a USB reflash (and losing every setting with the NVS erase).
    if (n < 3) {
      Serial.println("ERR usage: gw password reset | gw password \"<new>\"");
      return;
    }
    if (args[2] == "reset" && n == 3) {
      Serial.println(gwAuthResetPassword() ? "OK web password reset to the firmware default, sessions ended"
                                           : "ERR could not reset password");
      return;
    }
    String why;
    if (gwAuthSetPassword(args[2], why)) {
      gwAuthEndAllSessions();
      Serial.println("OK web password changed, sessions ended");
    } else {
      Serial.printf("ERR %s\n", why.c_str());
    }
    return;
  }
#if defined(GARNET_WEB_WIFI)
  if (cmd == "wifi") {
    if (n < 3 || args[2].length() == 0 || args[2].length() > 32) {
      Serial.println("ERR usage: gw wifi \"<ssid>\" \"<password>\" (SSID 1-32 chars)");
      return;
    }
    const String pass = n >= 4 ? args[3] : String();
    // Same rules as the web UI: WPA2 needs 8-63 characters, "" = open.
    if (pass.length() > 0 && (pass.length() < 8 || pass.length() > 63)) {
      Serial.println("ERR password must be 8-63 characters (or none for an open network)");
      return;
    }
    if (!gwWifiAddNet(args[2], pass)) {
      Serial.println("ERR could not store network");
      return;
    }
    Serial.printf("OK saved \"%s\", restarting to join it\n", args[2].c_str());
    Serial.flush();
    gwDeferReboot(500);
    return;
  }
  if (cmd == "forget") {
    if (n < 3) {
      Serial.println("ERR usage: gw forget \"<ssid>\"");
      return;
    }
    Serial.println(gwWifiForgetNet(args[2]) ? "OK forgotten" : "ERR not saved");
    return;
  }
#endif
  Serial.printf("ERR unknown command \"%s\" - try: gw help\n", cmd.c_str());
}

} // namespace

void gwSerialLoop() {
  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c < 0) break;
    if (c == '\r' || c == '\n') {
      if (overflow) {
        Serial.println("ERR line too long");
      } else if (lineLen > 0) {
        line[lineLen] = '\0';
        run(line);
      }
      lineLen = 0;
      overflow = false;
    } else if (lineLen < kMaxLine) {
      line[lineLen++] = (char)c;
    } else {
      overflow = true; // keep draining until end of line, then report once
    }
  }
}

#endif // GARNET_WEB_SERIAL
