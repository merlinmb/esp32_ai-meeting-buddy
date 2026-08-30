#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <SD.h>
#include <vector>
#include <functional>
#include "config.h"

// Connects to WiFi on demand, POSTs pending WAV files to the companion
// receiver script as multipart/form-data (streamed from SD, not buffered
// in RAM), then disconnects. Never runs during recording (see main.cpp's
// state machine) - WiFi during I2S capture risks dropped audio samples.
//
// Supports both http:// and https:// (via WiFiClientSecure with
// setInsecure() - the upload token already authenticates requests, so
// certificate pinning isn't worth the maintenance for this hobby device).

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

  bool uploadFile(const String &path, std::function<void(size_t, size_t)> onChunk = nullptr) {
    File probe = SD.open(path, FILE_READ);
    if (!probe) return false;
    size_t fileSize = probe.size();
    probe.close();

    const char *boundary = "AIMeetingBuddyBoundary";
    String head = String("--") + boundary + "\r\n" +
                  "Content-Disposition: form-data; name=\"audio\"; filename=\"" + baseName(path) + "\"\r\n" +
                  "Content-Type: audio/wav\r\n\r\n";
    String tail = String("\r\n--") + boundary + "--\r\n";

    return uploadFileStreamed(path, head, tail, fileSize, onChunk);
  }

  // Uploads every pending file; calls onProgress(doneCount, total) after each
  // file finishes, and onChunkProgress(sentBytes, fileSize) as each chunk of
  // the current file is sent, so a single large/slow file still shows the
  // transfer advancing instead of sitting static until it completes.
  void uploadAllPending(std::vector<String> &files, std::function<void(int, int)> onProgress = nullptr,
                        std::function<void(size_t, size_t)> onChunkProgress = nullptr) {
    int done = 0;
    for (auto &path : files) {
      if (uploadFile(path, onChunkProgress)) {
        moveToUploaded(path);
      }
      done++;
      if (onProgress) onProgress(done, (int)files.size());
    }
  }

 private:
  static String baseName(const String &path) {
    int idx = path.lastIndexOf('/');
    return idx >= 0 ? path.substring(idx + 1) : path;
  }

  // Moves a successfully-uploaded recording into /uploaded so it won't be
  // picked up by pendingFiles() again.
  static void moveToUploaded(const String &wavPath) {
    if (!SD.exists("/uploaded")) SD.mkdir("/uploaded");
    String dest = "/uploaded/" + baseName(wavPath);
    SD.remove(dest);  // rename() fails if the destination already exists
    SD.rename(wavPath, dest);
  }

  bool uploadFileStreamed(const String &path, const String &head, const String &tail, size_t fileSize,
                          std::function<void(size_t, size_t)> onChunk = nullptr) {
    String url = UPLOAD_SERVER_URL;
    bool useTls = url.startsWith("https://");
    if (!useTls && !url.startsWith("http://")) return false;
    String rest = url.substring(useTls ? 8 : 7);
    int slashIdx = rest.indexOf('/');
    String hostPort = slashIdx >= 0 ? rest.substring(0, slashIdx) : rest;
    String reqPath = slashIdx >= 0 ? rest.substring(slashIdx) : "/";
    int colonIdx = hostPort.indexOf(':');
    String host = colonIdx >= 0 ? hostPort.substring(0, colonIdx) : hostPort;
    int port = colonIdx >= 0 ? hostPort.substring(colonIdx + 1).toInt() : (useTls ? 443 : 80);

    WiFiClientSecure secureClient;
    WiFiClient plainClient;
    if (useTls) secureClient.setInsecure();
    WiFiClient &client = useTls ? (WiFiClient &)secureClient : plainClient;
    client.setTimeout(10000);
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
    uint8_t buf[8192];
    size_t sent = 0;
    while (f.available()) {
      size_t n = f.read(buf, sizeof(buf));
      client.write(buf, n);
      sent += n;
      if (onChunk) onChunk(sent, fileSize);
    }
    f.close();
    client.print(tail);

    unsigned long start = millis();
    while (client.connected() && !client.available() && (millis() - start) < 10000) delay(10);
    String statusLine = client.readStringUntil('\n');
    client.stop();

    return statusLine.indexOf("200") > 0;
  }
};
