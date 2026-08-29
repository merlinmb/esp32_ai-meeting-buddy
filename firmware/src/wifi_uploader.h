#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <SD.h>
#include <vector>
#include <functional>
#include "config.h"

// Connects to WiFi on demand, POSTs pending WAV files to the companion
// receiver script as multipart/form-data (streamed from SD, not buffered
// in RAM), then disconnects. Never runs during recording (see main.cpp's
// state machine) - WiFi during I2S capture risks dropped audio samples.
//
// Only plain http:// is supported here to keep things simple. If your
// server needs TLS, switch WiFiClient for WiFiClientSecure and add the
// server's certificate/fingerprint.
//
// Every blocking wait below is capped (see config.h's UPLOAD_*_TIMEOUT_MS)
// so a stalled link fails fast instead of hanging loop() long enough to
// trip the watchdog. Each file gets a few retries with a short backoff
// before this pass gives up on it; main.cpp surfaces a pass that leaves
// any file un-uploaded so the display can offer a manual retry.

class WifiUploader {
 public:
  bool connect() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
      delay(200);
    }
    return WiFi.status() == WL_CONNECTED;
  }

  void disconnect() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  bool uploadFile(const String &path) {
    File probe = SD.open(path, FILE_READ);
    if (!probe) return false;
    size_t fileSize = probe.size();
    probe.close();

    const char *boundary = "AIMeetingBuddyBoundary";
    String head = String("--") + boundary + "\r\n" +
                  "Content-Disposition: form-data; name=\"audio\"; filename=\"" + baseName(path) + "\"\r\n" +
                  "Content-Type: audio/wav\r\n\r\n";
    String tail = String("\r\n--") + boundary + "--\r\n";

    for (int attempt = 1; attempt <= UPLOAD_MAX_RETRIES; attempt++) {
      if (uploadFileStreamed(path, head, tail, fileSize)) return true;
      if (attempt < UPLOAD_MAX_RETRIES) delay(UPLOAD_RETRY_BACKOFF_MS);
    }
    return false;
  }

  // Uploads every pending file; calls onProgress(doneCount, total) after each.
  // Returns the count of files that failed all their retries (0 = clean pass).
  int uploadAllPending(std::vector<String> &files, std::function<void(int, int)> onProgress = nullptr) {
    int done = 0;
    int failed = 0;
    for (auto &path : files) {
      if (uploadFile(path)) {
        markDone(path);
      } else {
        failed++;
      }
      done++;
      if (onProgress) onProgress(done, (int)files.size());
    }
    return failed;
  }

 private:
  static String baseName(const String &path) {
    int idx = path.lastIndexOf('/');
    return idx >= 0 ? path.substring(idx + 1) : path;
  }

  static void markDone(const String &wavPath) {
    String donePath = wavPath.substring(0, wavPath.length() - 4) + ".done";
    File f = SD.open(donePath, FILE_WRITE);
    if (f) f.close();
  }

  bool uploadFileStreamed(const String &path, const String &head, const String &tail, size_t fileSize) {
    String url = UPLOAD_SERVER_URL;
    if (!url.startsWith("http://")) return false;
    String rest = url.substring(7);
    int slashIdx = rest.indexOf('/');
    String hostPort = slashIdx >= 0 ? rest.substring(0, slashIdx) : rest;
    String reqPath = slashIdx >= 0 ? rest.substring(slashIdx) : "/";
    int colonIdx = hostPort.indexOf(':');
    String host = colonIdx >= 0 ? hostPort.substring(0, colonIdx) : hostPort;
    int port = colonIdx >= 0 ? hostPort.substring(colonIdx + 1).toInt() : 80;

    WiFiClient client;
    // Bounds the TCP connect and (later) the final readStringUntil.
    client.setTimeout(UPLOAD_SOCKET_TIMEOUT_MS);
    if (!client.connect(host.c_str(), port)) return false;

    size_t totalLen = head.length() + fileSize + tail.length();

    client.printf("POST %s HTTP/1.1\r\n", reqPath.c_str());
    client.printf("Host: %s\r\n", host.c_str());
    client.println("Content-Type: multipart/form-data; boundary=AIMeetingBuddyBoundary");
    client.printf("Content-Length: %u\r\n", (unsigned)totalLen);
    client.printf("X-Upload-Token: %s\r\n", UPLOAD_TOKEN);
    client.println("Connection: close");
    client.println();
    client.print(head);

    File f = SD.open(path, FILE_READ);
    if (!f) { client.stop(); return false; }

    // Streams the body with a stall watchdog rather than a total-transfer
    // timeout: the clock resets after every chunk actually written, so a
    // long (but progressing) upload of a big recording is fine, while a
    // link that stops accepting data gets caught and aborted quickly
    // instead of blocking loop() indefinitely.
    bool streamOk = true;
    unsigned long lastProgressMs = millis();
    uint8_t buf[1024];
    while (f.available()) {
      if (!client.connected() ||
          (millis() - lastProgressMs) > UPLOAD_STALL_TIMEOUT_MS) {
        streamOk = false;
        break;
      }
      size_t n = f.read(buf, sizeof(buf));
      size_t written = client.write(buf, n);
      if (written != n) {
        streamOk = false;
        break;
      }
      lastProgressMs = millis();
      yield();  // let WiFi/lwIP and the watchdog run between chunks
    }
    f.close();

    if (!streamOk) {
      client.stop();
      return false;
    }

    client.print(tail);

    unsigned long start = millis();
    while (client.connected() && !client.available() &&
           (millis() - start) < UPLOAD_SOCKET_TIMEOUT_MS) {
      delay(10);
    }
    String statusLine = client.readStringUntil('\n');
    client.stop();

    return statusLine.indexOf("200") > 0;
  }
};
