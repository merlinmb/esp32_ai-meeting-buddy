#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <vector>

// Persists saved WiFi networks (SSID + password) in NVS flash via
// Preferences, so credentials survive reboots without depending on the SD
// card (WiFi needs to work even when there's no card inserted). Networks are
// stored as flat indexed keys (ssid0/pass0, ssid1/pass1, ...) rather than a
// JSON blob - a handful of string pairs doesn't need ArduinoJson as a new
// dependency.
//
// "Preferred" network = most recently saved/connected one (lastSsid), used
// to auto-reconnect on boot and from the "WiFi" menu item.

struct SavedNetwork {
  String ssid;
  String password;
};

class WifiStore {
 public:
  static const int kMaxNetworks = 5;

  std::vector<SavedNetwork> load() {
    Preferences prefs;
    prefs.begin(kNamespace, true);
    int count = prefs.getInt("count", 0);
    std::vector<SavedNetwork> out;
    for (int i = 0; i < count; i++) {
      SavedNetwork n;
      n.ssid = prefs.getString(ssidKey(i).c_str(), "");
      n.password = prefs.getString(passKey(i).c_str(), "");
      if (n.ssid.length()) out.push_back(n);
    }
    prefs.end();
    return out;
  }

  // Upserts (ssid, password) and makes it the preferred network. Evicts the
  // oldest entry if already at kMaxNetworks and ssid is new.
  void save(const String &ssid, const String &password) {
    if (!ssid.length()) return;
    std::vector<SavedNetwork> networks = load();

    int existingIdx = -1;
    for (size_t i = 0; i < networks.size(); i++) {
      if (networks[i].ssid == ssid) { existingIdx = (int)i; break; }
    }
    if (existingIdx >= 0) {
      networks[existingIdx].password = password;
      // Move to the end (most recent) so eviction drops the true oldest.
      SavedNetwork updated = networks[existingIdx];
      networks.erase(networks.begin() + existingIdx);
      networks.push_back(updated);
    } else {
      if ((int)networks.size() >= kMaxNetworks) networks.erase(networks.begin());
      networks.push_back({ssid, password});
    }

    writeAll(networks);
    Preferences prefs;
    prefs.begin(kNamespace, false);
    prefs.putString("lastSsid", ssid);
    prefs.end();
  }

  // Removes one saved network by SSID. If it was the preferred network,
  // the preferred SSID is cleared (falls back to config.h's default, same
  // as when nothing has ever been saved).
  void remove(const String &ssid) {
    std::vector<SavedNetwork> networks = load();
    for (size_t i = 0; i < networks.size(); i++) {
      if (networks[i].ssid == ssid) { networks.erase(networks.begin() + i); break; }
    }
    writeAll(networks);

    Preferences prefs;
    prefs.begin(kNamespace, false);
    if (prefs.getString("lastSsid", "") == ssid) {
      prefs.putString("lastSsid", networks.empty() ? "" : networks.back().ssid);
    }
    prefs.end();
  }

  bool findPassword(const String &ssid, String &outPassword) {
    for (auto &n : load()) {
      if (n.ssid == ssid) { outPassword = n.password; return true; }
    }
    return false;
  }

  // Last-connected network's SSID, or "" if none saved yet.
  String preferredSsid() {
    Preferences prefs;
    prefs.begin(kNamespace, true);
    String ssid = prefs.getString("lastSsid", "");
    prefs.end();
    return ssid;
  }

  void clear() {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    prefs.clear();
    prefs.end();
  }

 private:
  static constexpr const char *kNamespace = "wifinet";

  static String ssidKey(int i) { return "ssid" + String(i); }
  static String passKey(int i) { return "pass" + String(i); }

  void writeAll(const std::vector<SavedNetwork> &networks) {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    prefs.clear();
    prefs.putInt("count", (int)networks.size());
    for (size_t i = 0; i < networks.size(); i++) {
      prefs.putString(ssidKey((int)i).c_str(), networks[i].ssid);
      prefs.putString(passKey((int)i).c_str(), networks[i].password);
    }
    prefs.end();
  }
};
