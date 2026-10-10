#include "Arduino.h"
#include "WiFi.h"
#include "Preferences.h"
#include "../../config.h"
#include "../../secrets.h"
#include "settings.h"

static const char* NS = "palanote";

static String profKey(const char* kind, int i) { return String(kind) + String(i); }

static int storedProfileCount(Preferences& p) {
  int n = 0;
  for (int i = 0; i < WIFI_MAX_PROFILES; i++)
    if (p.getString(profKey("s", i).c_str(), "").length()) n++;
  return n;
}

void settingsWifiBegin() {
  Preferences p;
  p.begin(NS, true);
  int stored = storedProfileCount(p);
  if (stored == 0) {
    p.end();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    return;
  }

  // Candidates: stored profiles plus the compiled-in network as a fallback.
  String ssids[WIFI_MAX_PROFILES + 1], pass[WIFI_MAX_PROFILES + 1];
  int n = 0;
  for (int i = 0; i < WIFI_MAX_PROFILES; i++) {
    String s = p.getString(profKey("s", i).c_str(), "");
    if (!s.length()) continue;
    ssids[n] = s; pass[n] = p.getString(profKey("p", i).c_str(), ""); n++;
  }
  p.end();
  ssids[n] = WIFI_SSID; pass[n] = WIFI_PASS; n++;

  WiFi.mode(WIFI_STA);
  int found = WiFi.scanNetworks(false, false);
  int best = -1, bestRssi = -1000;
  for (int k = 0; k < found; k++) {
    for (int c = 0; c < n; c++) {
      if (WiFi.SSID(k) == ssids[c] && WiFi.RSSI(k) > bestRssi) { best = c; bestRssi = WiFi.RSSI(k); }
    }
  }
  WiFi.scanDelete();
  if (best < 0) best = n - 1;
  Serial.printf("[WiFi] using %s\n", ssids[best].c_str());
  WiFi.begin(ssids[best].c_str(), pass[best].c_str());
}

uint8_t settingsVolume() {
  Preferences p;
  p.begin(NS, true);
  int v = p.getUChar("vol", 85);
  p.end();
  return (uint8_t)constrain(v, 10, 100);
}

static void listProfiles() {
  Preferences p;
  p.begin(NS, true);
  int n = 0;
  for (int i = 0; i < WIFI_MAX_PROFILES; i++) {
    String s = p.getString(profKey("s", i).c_str(), "");
    if (s.length()) { Serial.printf("[Setup] wifi %d: %s\n", i + 1, s.c_str()); n++; }
  }
  p.end();
  Serial.printf("[Setup] %d stored profile(s); secrets.h network is always the fallback\n", n);
}

static void addProfile(const String& ssid, const String& pw) {
  Preferences p;
  p.begin(NS, false);
  int slot = -1;
  for (int i = 0; i < WIFI_MAX_PROFILES; i++) {
    String s = p.getString(profKey("s", i).c_str(), "");
    if (s == ssid) { slot = i; break; }
    if (!s.length() && slot < 0) slot = i;
  }
  if (slot < 0) { p.end(); Serial.println("[Setup] full - remove one with WIFI-ssid"); return; }
  p.putString(profKey("s", slot).c_str(), ssid);
  p.putString(profKey("p", slot).c_str(), pw);
  p.end();
  Serial.printf("[Setup] saved wifi '%s'\n", ssid.c_str());
}

static void removeProfile(const String& ssid) {
  Preferences p;
  p.begin(NS, false);
  bool removed = false;
  for (int i = 0; i < WIFI_MAX_PROFILES; i++) {
    if (p.getString(profKey("s", i).c_str(), "") == ssid) {
      p.remove(profKey("s", i).c_str()); p.remove(profKey("p", i).c_str()); removed = true;
    }
  }
  p.end();
  Serial.println(removed ? "[Setup] removed" : "[Setup] not found");
}

void settingsPollSerial() {
  if (!Serial.available()) return;
  String ln = Serial.readStringUntil('\n'); ln.trim();
  if (!ln.length()) return;

  if (ln.startsWith("WIFI+")) {
    String rest = ln.substring(5); rest.trim();
    int bar = rest.indexOf('|');
    String ssid = bar < 0 ? rest : rest.substring(0, bar);
    String pw   = bar < 0 ? "" : rest.substring(bar + 1);
    ssid.trim();
    if (ssid.length()) addProfile(ssid, pw);
    else Serial.println("[Setup] usage: WIFI+ ssid|password");
  } else if (ln.startsWith("WIFI-")) {
    String ssid = ln.substring(5); ssid.trim();
    removeProfile(ssid);
  } else if (ln == "WIFI?" || ln == "STATUS") {
    listProfiles();
    if (ln == "STATUS") Serial.printf("[Setup] volume %u, firmware " FIRMWARE_VERSION "\n", settingsVolume());
  } else if (ln.startsWith("VOL")) {
    int v = ln.substring(3).toInt();
    if (v < 10 || v > 100) { Serial.println("[Setup] usage: VOL 10..100"); return; }
    Preferences p; p.begin(NS, false); p.putUChar("vol", (uint8_t)v); p.end();
    Serial.printf("[Setup] volume %d\n", v);
  } else {
    Serial.println("[Setup] commands: WIFI+ ssid|pass, WIFI-ssid, WIFI?, VOL n, STATUS");
  }
}
