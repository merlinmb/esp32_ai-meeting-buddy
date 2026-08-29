#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <SD.h>
#include <vector>
#include <functional>
#include "config.h"

// Connects to WiFi on demand, then POSTs pending WAV files to the companion
// receiver script in small chunks (streamed from SD, not buffered in RAM),
// then disconnects. Never runs during recording (see main.cpp's state
// machine) - WiFi during I2S capture risks dropped audio samples.
//
// Only plain http:// is supported here to keep things simple. If your
// server needs TLS, switch WiFiClient for WiFiClientSecure and add the
// server's certificate/fingerprint.
//
// Uploads are resumable: the server is the only source of truth for how
// many bytes of a given file it already has (see receive_and_transcribe.py's
// /upload/status and /upload/chunk), and this class always asks (or reads
// the previous chunk's response) rather than assuming - so a meeting
// recording that's 2+ hours (100+ MB) resumes from wherever it left off
// after a dropped connection, a device reboot, or the next periodic retry,
// instead of restarting from byte 0. Every blocking wait is capped (see
// config.h's UPLOAD_*_TIMEOUT_MS) so a stalled link fails a chunk fast
// instead of hanging loop() long enough to trip the watchdog; only the
// failed chunk needs retrying, not the whole file.

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
    String host, reqPath;
    int port;
    if (!parseUrl(host, port, reqPath)) return false;

    File probe = SD.open(path, FILE_READ);
    if (!probe) return false;
    size_t fileSize = probe.size();
    probe.close();

    String fileName = baseName(path);

    long resumeFrom = queryResumeOffset(host, port, reqPath, fileName);
    if (resumeFrom < 0) resumeFrom = 0;  // couldn't ask - start from 0, a 409 will correct us if that's wrong
    if ((size_t)resumeFrom >= fileSize) return true;  // server already has the whole file

    File f = SD.open(path, FILE_READ);
    if (!f) return false;

    size_t offset = (size_t)resumeFrom;
    int consecutiveFailures = 0;
    bool ok = true;
    while (offset < fileSize) {
      size_t chunkLen = min((size_t)UPLOAD_CHUNK_BYTES, fileSize - offset);
      bool isFinal = (offset + chunkLen) >= fileSize;

      long serverBytes = -1;
      bool complete = false;
      int statusCode = 0;
      bool sent = sendChunk(host, port, reqPath, fileName, f, offset, chunkLen, isFinal,
                             statusCode, serverBytes, complete);

      if (sent && statusCode == 200 && serverBytes == (long)(offset + chunkLen)) {
        offset += chunkLen;
        consecutiveFailures = 0;
        if (complete) break;
        continue;
      }

      if (sent && statusCode == 409 && serverBytes >= 0) {
        // Server disagrees on where we are (e.g. a previous ack got lost
        // over a dropped connection) - trust it and resync instead of
        // treating this as a failure.
        offset = (size_t)serverBytes;
        consecutiveFailures = 0;
        if (offset >= fileSize) break;
        continue;
      }

      consecutiveFailures++;
      if (consecutiveFailures > UPLOAD_MAX_RETRIES) { ok = false; break; }
      delay(UPLOAD_RETRY_BACKOFF_MS);
    }

    f.close();
    return ok;
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

  bool parseUrl(String &host, int &port, String &reqPath) {
    String url = UPLOAD_SERVER_URL;
    if (!url.startsWith("http://")) return false;
    String rest = url.substring(7);
    int slashIdx = rest.indexOf('/');
    String hostPort = slashIdx >= 0 ? rest.substring(0, slashIdx) : rest;
    reqPath = slashIdx >= 0 ? rest.substring(slashIdx) : "/";
    int colonIdx = hostPort.indexOf(':');
    host = colonIdx >= 0 ? hostPort.substring(0, colonIdx) : hostPort;
    port = colonIdx >= 0 ? hostPort.substring(colonIdx + 1).toInt() : 80;
    return true;
  }

  // Reads a "HTTP/1.1 <code> ..." status line, skips headers to the blank
  // line, then reads whatever's left as the body. Our device-facing
  // endpoints only ever return a short plain-text body (see
  // receive_and_transcribe.py), so this doesn't need to understand
  // Content-Length or chunked transfer encoding on the way back.
  static bool readHttpResponse(WiFiClient &client, unsigned long deadlineMs, int &statusCode, String &body) {
    while (client.connected() && !client.available() && millis() < deadlineMs) delay(10);
    if (!client.available()) return false;

    String statusLine = client.readStringUntil('\n');
    int sp1 = statusLine.indexOf(' ');
    int sp2 = sp1 >= 0 ? statusLine.indexOf(' ', sp1 + 1) : -1;
    if (sp1 < 0 || sp2 < 0) return false;
    statusCode = statusLine.substring(sp1 + 1, sp2).toInt();

    while (millis() < deadlineMs) {
      if (!client.connected() && !client.available()) break;
      String line = client.readStringUntil('\n');
      if (line.length() == 0 || line == "\r") break;  // blank line = end of headers
    }

    body = "";
    while ((client.connected() || client.available()) && millis() < deadlineMs) {
      while (client.available()) body += (char)client.read();
      if (!client.connected() && !client.available()) break;
      delay(5);
    }
    return true;
  }

  // Returns bytes already stored server-side for fileName (0 if none), or
  // -1 on any network/parse failure (the caller falls back to offset 0 and
  // lets a 409 on the first chunk correct it).
  long queryResumeOffset(const String &host, int port, const String &reqPath, const String &fileName) {
    WiFiClient client;
    client.setTimeout(UPLOAD_SOCKET_TIMEOUT_MS);
    if (!client.connect(host.c_str(), port)) return -1;

    // Device filenames are RTC-generated (digits/underscores only), so no
    // URL-encoding is needed here.
    String path = reqPath + "/status?name=" + fileName;
    client.printf("GET %s HTTP/1.1\r\n", path.c_str());
    client.printf("Host: %s\r\n", host.c_str());
    client.printf("X-Upload-Token: %s\r\n", UPLOAD_TOKEN);
    client.println("Connection: close");
    client.println();

    int statusCode = 0;
    String body;
    bool ok = readHttpResponse(client, millis() + UPLOAD_SOCKET_TIMEOUT_MS, statusCode, body);
    client.stop();
    if (!ok || statusCode != 200) return -1;
    return body.toInt();
  }

  // Sends [offset, offset+len) of the open file as one chunk POST. Returns
  // true whenever a well-formed HTTP response came back (including a 409
  // offset-mismatch - the caller resyncs to serverBytesReceived rather than
  // treating that as a failure); returns false only when no usable response
  // came back at all (dropped connection, stalled write, timeout).
  bool sendChunk(const String &host, int port, const String &reqPath, const String &fileName,
                 File &f, size_t offset, size_t len, bool isFinal,
                 int &statusCode, long &serverBytesReceived, bool &complete) {
    WiFiClient client;
    client.setTimeout(UPLOAD_SOCKET_TIMEOUT_MS);
    if (!client.connect(host.c_str(), port)) return false;

    String path = reqPath + "/chunk";
    client.printf("POST %s HTTP/1.1\r\n", path.c_str());
    client.printf("Host: %s\r\n", host.c_str());
    client.println("Content-Type: application/octet-stream");
    client.printf("Content-Length: %u\r\n", (unsigned)len);
    client.printf("X-Upload-Token: %s\r\n", UPLOAD_TOKEN);
    client.printf("X-File-Name: %s\r\n", fileName.c_str());
    client.printf("X-Chunk-Offset: %u\r\n", (unsigned)offset);
    client.printf("X-Upload-Final: %s\r\n", isFinal ? "1" : "0");
    client.println("Connection: close");
    client.println();

    f.seek(offset);

    // Same stall-watchdog approach as before, just scoped to one chunk now:
    // the clock resets on every buffer actually written, so it only fires
    // if the link genuinely stops accepting data mid-chunk.
    bool streamOk = true;
    unsigned long lastProgressMs = millis();
    size_t sentBytes = 0;
    uint8_t buf[1024];
    while (sentBytes < len) {
      if (!client.connected() || (millis() - lastProgressMs) > UPLOAD_STALL_TIMEOUT_MS) {
        streamOk = false;
        break;
      }
      size_t want = min((size_t)sizeof(buf), len - sentBytes);
      size_t n = f.read(buf, want);
      if (n == 0) { streamOk = false; break; }  // unexpected SD read failure
      size_t written = client.write(buf, n);
      if (written != n) { streamOk = false; break; }
      sentBytes += n;
      lastProgressMs = millis();
      yield();  // let WiFi/lwIP and the watchdog run between buffers
    }

    if (!streamOk) { client.stop(); return false; }

    String body;
    bool ok = readHttpResponse(client, millis() + UPLOAD_SOCKET_TIMEOUT_MS, statusCode, body);
    client.stop();
    if (!ok) return false;

    // Body is "<bytesReceived>" or "<bytesReceived> COMPLETE"; toInt() reads
    // the leading digits and ignores the trailing marker either way.
    complete = body.indexOf("COMPLETE") >= 0;
    serverBytesReceived = body.toInt();
    return true;
  }
};
