#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SD.h>
#include <M5Unified.h>
#include "config.h"
#include "wifi_store.h"
#include "wav_recorder.h"

// Serves a small dashboard + JSON API over WiFi: recordings (list/download/
// delete), live system info (heap, battery, SD, WiFi), and WiFi network
// preferences (list/add/edit/delete saved networks) backed by WifiStore.
//
// Started once WiFi associates (see main.cpp) and stopped on disconnect -
// it's meaningless without a link, and WebServer holds a listening socket
// that's cheapest to just not have open while offline. handleClient() is
// pumped from loop() only outside RECORDING, so HTTP handling never competes
// with the audio-to-SD path for CPU/stack.

class DeviceWebServer {
 public:
  void begin() {
    if (_started) return;
    _server.on("/", HTTP_GET, [this]() { handleRoot(); });
    _server.on("/api/status", HTTP_GET, [this]() { handleStatus(); });
    _server.on("/api/recordings", HTTP_GET, [this]() { handleListRecordings(); });
    _server.on("/api/recordings/delete", HTTP_POST, [this]() { handleDeleteRecording(); });
    _server.on("/download", HTTP_GET, [this]() { handleDownload(); });
    _server.on("/api/wifi", HTTP_GET, [this]() { handleWifiList(); });
    _server.on("/api/wifi", HTTP_POST, [this]() { handleWifiSave(); });
    _server.on("/api/wifi/delete", HTTP_POST, [this]() { handleWifiDelete(); });
    _server.onNotFound([this]() { _server.send(404, "text/plain", "Not found"); });
    _server.begin();
    _started = true;
  }

  void end() {
    if (!_started) return;
    _server.stop();
    _started = false;
  }

  bool isRunning() const { return _started; }

  void handleClient() {
    if (_started) _server.handleClient();
  }

 private:
  WebServer _server{80};
  bool _started = false;

  // ---- Pages -------------------------------------------------------------

  void handleRoot() {
    _server.send(200, "text/html", kIndexHtml);
  }

  // ---- JSON API ------------------------------------------------------------

  static String jsonEscape(const String &s) {
    String out;
    out.reserve(s.length() + 4);
    for (size_t i = 0; i < s.length(); i++) {
      char c = s[i];
      if (c == '"' || c == '\\') out += '\\';
      out += c;
    }
    return out;
  }

  void handleStatus() {
    bool wifiConnected = WiFi.status() == WL_CONNECTED;
    bool sdOk = SD.cardType() != CARD_NONE;
    uint64_t sdTotal = sdOk ? SD.totalBytes() : 0;
    uint64_t sdUsed = sdOk ? SD.usedBytes() : 0;
    int pendingCount = sdOk ? (int)WavRecorder::pendingFiles().size() : 0;

    String json = "{";
    json += "\"deviceVersion\":\"" + jsonEscape(DEVICE_VERSION) + "\",";
    json += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"totalHeap\":" + String(ESP.getHeapSize()) + ",";
    json += "\"freePsram\":" + String(ESP.getFreePsram()) + ",";
    json += "\"battery\":" + String(constrain(M5.Power.getBatteryLevel(), 0, 100)) + ",";
    json += "\"charging\":" + String(M5.Power.isCharging() == m5::Power_Class::is_charging_t::is_charging ? "true" : "false") + ",";
    json += "\"wifiConnected\":" + String(wifiConnected ? "true" : "false") + ",";
    json += "\"wifiSsid\":\"" + jsonEscape(wifiConnected ? WiFi.SSID() : "") + "\",";
    json += "\"wifiRssi\":" + String(wifiConnected ? WiFi.RSSI() : 0) + ",";
    json += "\"wifiIp\":\"" + (wifiConnected ? WiFi.localIP().toString() : String("")) + "\",";
    json += "\"sdOk\":" + String(sdOk ? "true" : "false") + ",";
    json += "\"sdTotalBytes\":" + String((unsigned long long)sdTotal) + ",";
    json += "\"sdUsedBytes\":" + String((unsigned long long)sdUsed) + ",";
    json += "\"pendingUploads\":" + String(pendingCount) + ",";
    json += "\"uptimeMs\":" + String(millis());
    json += "}";
    _server.send(200, "application/json", json);
  }

  // Lists recordings from both / (pending) and /uploaded, newest-first by
  // name (filenames are timestamp-based, so lexical order is chronological).
  void handleListRecordings() {
    String json = "[";
    bool first = true;
    for (auto &dir : {"/", "/uploaded"}) {
      File root = SD.open(dir);
      if (!root) continue;
      String dirPrefix = (String(dir) == "/") ? "/" : (String(dir) + "/");
      File f = root.openNextFile();
      while (f) {
        String base = baseName(f.name());  // f.name() may or may not include the dir prefix depending on SD lib version
        bool isDir = f.isDirectory();
        size_t size = f.size();
        if (!isDir && base.endsWith(".wav")) {
          String path = dirPrefix + base;
          if (!first) json += ",";
          first = false;
          json += "{";
          json += "\"name\":\"" + jsonEscape(base) + "\",";
          json += "\"path\":\"" + jsonEscape(path) + "\",";
          json += "\"sizeBytes\":" + String((unsigned long)size) + ",";
          json += "\"uploaded\":" + String(String(dir) == "/uploaded" ? "true" : "false");
          json += "}";
        }
        f = root.openNextFile();
      }
    }
    json += "]";
    _server.send(200, "application/json", json);
  }

  void handleDownload() {
    if (!_server.hasArg("name")) { _server.send(400, "text/plain", "Missing name"); return; }
    String path = resolveRecordingPath(_server.arg("name"));
    if (!path.length() || !SD.exists(path)) { _server.send(404, "text/plain", "Not found"); return; }

    File f = SD.open(path, FILE_READ);
    if (!f) { _server.send(500, "text/plain", "Open failed"); return; }
    _server.sendHeader("Content-Disposition", "attachment; filename=\"" + baseName(path) + "\"");
    _server.streamFile(f, "audio/wav");
    f.close();
  }

  void handleDeleteRecording() {
    if (!_server.hasArg("name")) { _server.send(400, "text/plain", "Missing name"); return; }
    String path = resolveRecordingPath(_server.arg("name"));
    if (!path.length() || !SD.exists(path)) { _server.send(404, "text/plain", "Not found"); return; }
    bool ok = SD.remove(path);
    _server.send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
  }

  // Recording names are plain timestamp basenames (no path separators), so a
  // stored file is found by checking / then /uploaded rather than trusting a
  // client-supplied path.
  String resolveRecordingPath(const String &name) {
    String base = baseName(name);
    if (!base.endsWith(".wav")) return "";
    if (SD.exists("/" + base)) return "/" + base;
    if (SD.exists("/uploaded/" + base)) return "/uploaded/" + base;
    return "";
  }

  static String baseName(const String &path) {
    int idx = path.lastIndexOf('/');
    return idx >= 0 ? path.substring(idx + 1) : path;
  }

  // ---- WiFi preferences ----------------------------------------------------

  void handleWifiList() {
    String preferred = _wifiStore.preferredSsid();
    String json = "{\"preferred\":\"" + jsonEscape(preferred) + "\",\"networks\":[";
    auto networks = _wifiStore.load();
    for (size_t i = 0; i < networks.size(); i++) {
      if (i) json += ",";
      json += "{\"ssid\":\"" + jsonEscape(networks[i].ssid) + "\"}";
    }
    json += "]}";
    _server.send(200, "application/json", json);
  }

  void handleWifiSave() {
    if (!_server.hasArg("ssid")) { _server.send(400, "application/json", "{\"ok\":false,\"error\":\"Missing ssid\"}"); return; }
    String ssid = _server.arg("ssid");
    String password = _server.arg("password");
    if (!ssid.length()) { _server.send(400, "application/json", "{\"ok\":false,\"error\":\"Empty ssid\"}"); return; }
    _wifiStore.save(ssid, password);
    _server.send(200, "application/json", "{\"ok\":true}");
  }

  void handleWifiDelete() {
    if (!_server.hasArg("ssid")) { _server.send(400, "application/json", "{\"ok\":false,\"error\":\"Missing ssid\"}"); return; }
    String ssid = _server.arg("ssid");
    auto networks = _wifiStore.load();
    bool found = false;
    for (auto &n : networks) if (n.ssid == ssid) { found = true; break; }
    if (!found) { _server.send(404, "application/json", "{\"ok\":false,\"error\":\"Not found\"}"); return; }
    _wifiStore.remove(ssid);
    _server.send(200, "application/json", "{\"ok\":true}");
  }

  WifiStore _wifiStore;

  static const char kIndexHtml[] PROGMEM;
};

#include "web_server_html.h"
