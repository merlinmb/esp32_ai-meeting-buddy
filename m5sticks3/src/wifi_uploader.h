#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <SD.h>
#include <vector>
#include <functional>
#include <algorithm>
#include "config.h"

// Connects to WiFi on demand, then POSTs pending WAV files to the companion
// receiver script in small chunks (streamed from SD, not buffered in RAM),
// then disconnects.
//
// Uploads are resumable: the server is the only source of truth for how many
// bytes of a given file it already has (see receive_and_transcribe.py's
// /upload/status and /upload/chunk), and this class always asks (or reads
// the previous chunk's response) rather than assuming - so a long recording
// resumes from wherever it left off after a dropped connection instead of
// restarting from byte 0. See config.h's UPLOAD_CHUNK_BYTES and friends for
// the tuning knobs.
//
// One TLS/TCP connection is kept open (HTTP keep-alive) across the status
// check and every chunk of a given file, instead of reconnecting per
// request - each TLS handshake costs the better part of a second, and on a
// mesh WiFi network that overhead was enough for the device to be roamed to
// a different mesh node mid-upload (a hard disconnect on ESP32, since it
// doesn't support seamless 802.11r/k/v roaming), destroying whatever
// connection was in flight. Fewer, longer-lived connections means fewer
// windows where that can happen. If the connection does drop anyway - a
// roam, a genuine network blip - a fresh one is opened for the next
// chunk/retry and the resumable protocol picks up from the last
// server-acked offset, so at most one chunk is lost rather than the whole
// file.
//
// Supports both http:// and https:// (via WiFiClientSecure with
// setInsecure() - the upload token already authenticates requests, so
// certificate pinning isn't worth the maintenance for this hobby device).

class WifiUploader {
 public:
  bool connect() { return connect(WIFI_SSID, WIFI_PASSWORD); }

  bool connect(const String &ssid, const String &password) {
    WiFi.mode(WIFI_STA);
    Serial.println("WiFi MAC: " + WiFi.macAddress());
    // Default modem-sleep power saving cycles the radio in and out of sleep
    // between packets, which some APs handle poorly under bursty small-
    // request traffic - that was showing up as random mid-upload
    // "STA Disconnected ... Reason: 8" association drops. Keep the radio
    // fully awake for the (short) lifetime of an upload session.
    WiFi.setSleep(false);
    WiFi.setHostname("esp32-aimeeting-buddy");
    WiFi.begin(ssid.c_str(), password.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
      delay(200);
    }
    bool connected = WiFi.status() == WL_CONNECTED;
    if (connected) {
      Serial.println("WiFi IP: " + WiFi.localIP().toString());
    }
    return connected;
  }

  void disconnect() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  // Scans for nearby APs, sorted strongest-first, deduped by SSID (an AP
  // with multiple radios/bands shows up once per band otherwise), and
  // skipping hidden (empty-SSID) networks - there'd be nothing to select
  // to connect to one anyway.
  bool scanNetworks(std::vector<String> &outSsids) {
    outSsids.clear();
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks();
    if (n < 0) return false;

    std::vector<std::pair<int32_t, String>> byRssi;
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);
      if (!ssid.length()) continue;
      bool dup = false;
      for (auto &p : byRssi) {
        if (p.second == ssid) { dup = true; break; }
      }
      if (dup) continue;
      byRssi.push_back({WiFi.RSSI(i), ssid});
    }
    WiFi.scanDelete();

    std::sort(byRssi.begin(), byRssi.end(),
              [](const std::pair<int32_t, String> &a, const std::pair<int32_t, String> &b) {
                return a.first > b.first;
              });
    for (auto &p : byRssi) outSsids.push_back(p.second);
    return true;
  }

  // Uploads one file in UPLOAD_CHUNK_BYTES chunks, resuming from whatever
  // offset the server reports it already has. onChunk(sentBytes, fileSize)
  // is called after each chunk lands so callers can show progress mid-file.
  bool uploadFile(const String &path, std::function<void(size_t, size_t)> onChunk = nullptr) {
    String host, reqPath;
    int port;
    bool useTls;
    if (!parseUrl(host, port, reqPath, useTls)) return false;

    File probe = SD.open(path, FILE_READ);
    if (!probe) return false;
    size_t fileSize = probe.size();
    probe.close();

    String fileName = baseName(path);
    Serial.println("uploadFile: " + fileName + " (" + String((unsigned)fileSize) + " bytes)");

    Session session;
    long resumeFrom = queryResumeOffset(session, host, port, reqPath, fileName, useTls);
    if (resumeFrom < 0) {
      Serial.println("uploadFile: status query failed, starting from 0");
      resumeFrom = 0;  // couldn't ask - start from 0, a 409 will correct us if that's wrong
    }
    if ((size_t)resumeFrom >= fileSize) { session.close(); return true; }  // server already has it all

    File f = SD.open(path, FILE_READ);
    if (!f) { session.close(); return false; }

    size_t offset = (size_t)resumeFrom;
    int consecutiveFailures = 0;
    bool ok = true;
    long meetingId = -1;  // set from the response that finalizes the file
    while (offset < fileSize) {
      size_t chunkLen = min((size_t)UPLOAD_CHUNK_BYTES, fileSize - offset);
      bool isFinal = (offset + chunkLen) >= fileSize;

      long serverBytes = -1;
      bool complete = false;
      int statusCode = 0;
      long chunkMeetingId = -1;
      bool sent = sendChunk(session, host, port, reqPath, fileName, f, offset, chunkLen, isFinal, useTls,
                            statusCode, serverBytes, complete, chunkMeetingId);
      if (chunkMeetingId >= 0) meetingId = chunkMeetingId;

      if (sent && statusCode == 200 && serverBytes == (long)(offset + chunkLen)) {
        offset += chunkLen;
        consecutiveFailures = 0;
        if (onChunk) onChunk(offset, fileSize);
        if (complete) break;
        continue;
      }

      if (sent && statusCode == 409 && serverBytes >= 0) {
        // Server disagrees on where we are (e.g. a previous ack got lost
        // over a dropped connection) - trust it and resync instead of
        // treating this as a failure.
        offset = (size_t)serverBytes;
        consecutiveFailures = 0;
        if (onChunk) onChunk(offset, fileSize);
        if (offset >= fileSize) break;
        continue;
      }

      consecutiveFailures++;
      Serial.println("uploadFile: chunk at offset " + String((unsigned)offset) + " failed (sent=" +
                     String(sent) + " status=" + String(statusCode) + " serverBytes=" + String(serverBytes) +
                     "), attempt " + String(consecutiveFailures) + "/" + String((int)UPLOAD_MAX_RETRIES));
      if (consecutiveFailures > UPLOAD_MAX_RETRIES) {
        Serial.println("uploadFile: giving up on " + fileName + " after " + String(consecutiveFailures) + " consecutive failures");
        ok = false;
        break;
      }
      delay(UPLOAD_RETRY_BACKOFF_MS);
    }

    f.close();
    // Tagging is a separate request to the receiver, deliberately after the
    // upload rather than part of it: the WAV is already safely stored and
    // queued at this point, so a failed tag call costs nothing more than the
    // server's own default tag. Never let it turn a good upload into a
    // failure the device would then retry from scratch.
    if (ok && meetingId >= 0) {
      if (!tagMeeting(session, host, port, useTls, meetingId, UPLOAD_TAG)) {
        Serial.println("uploadFile: tagging meeting " + String(meetingId) + " as '" + String(UPLOAD_TAG) +
                       "' failed - server keeps its default tag");
      }
    }
    session.close();
    return ok;
  }

  // Uploads every pending file; calls onProgress(doneCount, succeededCount,
  // total) after each file finishes, and onChunkProgress(sentBytes, fileSize)
  // as each chunk of the current file is sent, so a single large/slow file
  // still shows the transfer advancing instead of sitting static until it
  // completes. doneCount is attempts, not successes - callers that want to
  // show failures distinctly should compare it against succeededCount.
  // Returns how many files actually succeeded (moved to /uploaded) - a file
  // that fails (server unreachable/rejects it) is left pending for the next
  // retry rather than counted, so callers can tell a real success from a
  // no-op retry that just re-attempted the same stuck files.
  int uploadAllPending(std::vector<String> &files, std::function<void(int, int, int)> onProgress = nullptr,
                       std::function<void(size_t, size_t)> onChunkProgress = nullptr) {
    int done = 0;
    int succeeded = 0;
    for (auto &path : files) {
      if (uploadFile(path, onChunkProgress) && moveToUploaded(path)) {
        succeeded++;
      }
      done++;
      if (onProgress) onProgress(done, succeeded, (int)files.size());
    }
    return succeeded;
  }

 private:
  static String baseName(const String &path) {
    int idx = path.lastIndexOf('/');
    return idx >= 0 ? path.substring(idx + 1) : path;
  }

  // Moves a successfully-uploaded recording into /uploaded so it won't be
  // picked up by pendingFiles() again. Returns false if the move didn't
  // actually happen (caller should then leave the file pending rather than
  // reporting a success that a silent rename failure would otherwise hide -
  // pendingFiles() would just pick it back up and re-upload it anyway).
  static bool moveToUploaded(const String &wavPath) {
    if (!SD.exists("/uploaded") && !SD.mkdir("/uploaded")) {
      Serial.println("moveToUploaded: mkdir /uploaded failed");
      return false;
    }
    String dest = "/uploaded/" + baseName(wavPath);
    if (SD.exists(dest)) SD.remove(dest);  // rename() fails if the destination already exists
    if (!SD.rename(wavPath, dest)) {
      Serial.println("moveToUploaded: rename failed for " + wavPath);
      return false;
    }
    return true;
  }

  bool parseUrl(String &host, int &port, String &reqPath, bool &useTls) {
    String url = UPLOAD_SERVER_URL;
    useTls = url.startsWith("https://");
    if (!useTls && !url.startsWith("http://")) return false;
    String rest = url.substring(useTls ? 8 : 7);
    int slashIdx = rest.indexOf('/');
    String hostPort = slashIdx >= 0 ? rest.substring(0, slashIdx) : rest;
    reqPath = slashIdx >= 0 ? rest.substring(slashIdx) : "/";
    int colonIdx = hostPort.indexOf(':');
    host = colonIdx >= 0 ? hostPort.substring(0, colonIdx) : hostPort;
    port = colonIdx >= 0 ? hostPort.substring(colonIdx + 1).toInt() : (useTls ? 443 : 80);
    return true;
  }

  // Wraps one kept-alive connection to the upload host, reused across the
  // status check and every chunk of a file. A dropped/closed connection
  // (roam, server timing out an idle keep-alive, etc) is detected in
  // ensureOpen() and transparently replaced with a fresh one - callers just
  // always go through ensureOpen() before using client().
  struct Session {
    WiFiClient *client = nullptr;
    bool useTls = false;

    bool ensureOpen(const String &host, int port, bool tls) {
      useTls = tls;
      if (client && client->connected()) return true;
      close();
      client = useTls ? (WiFiClient *)new WiFiClientSecure() : new WiFiClient();
      if (useTls) ((WiFiClientSecure *)client)->setInsecure();
      client->setTimeout(UPLOAD_SOCKET_TIMEOUT_MS);
      if (!client->connect(host.c_str(), port)) {
        close();
        return false;
      }
      return true;
    }

    void close() {
      if (client) {
        client->stop();
        delete client;
        client = nullptr;
      }
    }
  };

  // Reads one line, blocking only up to whatever's left of deadlineMs -
  // unlike Stream::readStringUntil(), which uses its own fixed internal
  // timeout (Arduino default 1000ms via setTimeout()) regardless of
  // deadlineMs. That mismatch was the actual bug behind most "resumable"
  // uploads quietly corrupting: if the server took just over a second to
  // flush the next header line (a slow/congested link, not a real failure),
  // readStringUntil() gave up and returned "" - which the old code read as
  // "blank line, end of headers", so it stopped parsing headers early,
  // missed Content-Length, and misaligned every byte read after that on a
  // reused keep-alive connection. Returns "" (with ok=false) only on a real
  // deadline expiry or disconnect, never on an ordinary slow line.
  static String readLineWithDeadline(WiFiClient &client, unsigned long deadlineMs, bool &ok) {
    String line;
    line.reserve(64);
    for (;;) {
      while (client.available()) {
        char c = (char)client.read();
        if (c == '\n') { ok = true; return line; }
        if (c != '\r') line += c;
      }
      if (!client.connected() && !client.available()) { ok = false; return line; }
      if (millis() >= deadlineMs) { ok = false; return line; }
      delay(2);
    }
  }

  // Reads a "HTTP/1.1 <code> ..." status line, then headers (tracking
  // Content-Length and any explicit "Connection: close"), then exactly
  // Content-Length bytes of body - required for keep-alive, since without a
  // length the only way to know a response has ended is the connection
  // closing, which defeats reusing it. keepAlive is set to false if the
  // server body lacked a Content-Length (can't safely reuse the connection)
  // or asked to close it itself.
  static bool readHttpResponse(WiFiClient &client, unsigned long deadlineMs, int &statusCode, String &body,
                               bool &keepAlive) {
    while (client.connected() && !client.available() && millis() < deadlineMs) {
      delay(10);
    }
    if (!client.available()) return false;

    bool lineOk = false;
    String statusLine = readLineWithDeadline(client, deadlineMs, lineOk);
    if (!lineOk || !statusLine.startsWith("HTTP/")) return false;
    int sp1 = statusLine.indexOf(' ');
    int sp2 = sp1 >= 0 ? statusLine.indexOf(' ', sp1 + 1) : -1;
    if (sp1 < 0 || sp2 < 0) return false;
    statusCode = statusLine.substring(sp1 + 1, sp2).toInt();

    long contentLength = -1;
    keepAlive = true;
    for (;;) {
      String line = readLineWithDeadline(client, deadlineMs, lineOk);
      if (!lineOk) return false;  // real timeout/disconnect mid-headers - don't trust this connection's framing
      if (line.length() == 0) break;  // blank line = end of headers
      String lower = line;
      lower.toLowerCase();
      if (lower.startsWith("content-length:")) {
        contentLength = line.substring(line.indexOf(':') + 1).toInt();
      } else if (lower.startsWith("connection:") && lower.indexOf("close") >= 0) {
        keepAlive = false;
      }
    }

    body = "";
    if (contentLength < 0) {
      // No length given - can't safely leave the connection open for reuse,
      // so drain until close instead (same behavior as the old per-request
      // connection model).
      keepAlive = false;
      while ((client.connected() || client.available()) && millis() < deadlineMs) {
        while (client.available()) body += (char)client.read();
        if (!client.connected() && !client.available()) break;
        delay(5);
      }
      return true;
    }

    while ((long)body.length() < contentLength && millis() < deadlineMs) {
      while (client.available() && (long)body.length() < contentLength) body += (char)client.read();
      if (!client.connected() && !client.available() && (long)body.length() < contentLength) break;
      if ((long)body.length() < contentLength) delay(5);
    }
    return (long)body.length() == contentLength;
  }

  // Returns bytes already stored server-side for fileName (0 if none), or
  // -1 on any network/parse failure (the caller falls back to offset 0 and
  // lets a 409 on the first chunk correct it).
  long queryResumeOffset(Session &session, const String &host, int port, const String &reqPath,
                         const String &fileName, bool useTls) {
    if (!session.ensureOpen(host, port, useTls)) return -1;

    // Device filenames are RTC-generated (digits/underscores only), so no
    // URL-encoding is needed here.
    String path = reqPath + "/status?name=" + fileName;
    // Single write() for the whole header block - each client->print*() call
    // otherwise becomes its own TLS record, adding needless latency.
    String req = "GET " + path + " HTTP/1.1\r\n" +
                "Host: " + host + "\r\n" +
                "X-Upload-Token: " + UPLOAD_TOKEN + "\r\n" +
                "Connection: keep-alive\r\n\r\n";
    session.client->write((const uint8_t *)req.c_str(), req.length());

    int statusCode = 0;
    String body;
    bool keepAlive = false;
    bool ok = readHttpResponse(*session.client, millis() + UPLOAD_SOCKET_TIMEOUT_MS, statusCode, body, keepAlive);
    if (!keepAlive) session.close();
    if (!ok || statusCode != 200) return -1;
    return body.toInt();
  }

  // POSTs the configured tag for a recording the server has just accepted.
  // The endpoint lives beside /upload rather than under it, so this derives
  // "/api/meetings/<id>/tag" from UPLOAD_SERVER_URL's path by dropping the
  // trailing "/upload" - that way a server hosted under a sub-path still
  // resolves correctly instead of assuming the API sits at the root.
  bool tagMeeting(Session &session, const String &host, int port, bool useTls, long meetingId, const String &tag) {
    if (!session.ensureOpen(host, port, useTls)) return false;
    WiFiClient &client = *session.client;

    String path = uploadPathPrefix() + "/api/meetings/" + String(meetingId) + "/tag";
    String payload = "{\"tag\":\"" + tag + "\"}";

    // One write() for headers+body - see the matching comment in queryResumeOffset().
    String req = "POST " + path + " HTTP/1.1\r\n" +
                "Host: " + host + "\r\n" +
                "Content-Type: application/json\r\n" +
                "Content-Length: " + String(payload.length()) + "\r\n" +
                "X-Upload-Token: " + UPLOAD_TOKEN + "\r\n" +
                "Connection: keep-alive\r\n\r\n" + payload;
    client.write((const uint8_t *)req.c_str(), req.length());

    int statusCode = 0;
    String body;
    bool keepAlive = false;
    bool ok = readHttpResponse(client, millis() + UPLOAD_SOCKET_TIMEOUT_MS, statusCode, body, keepAlive);
    if (!keepAlive) session.close();
    if (!ok || statusCode != 200) return false;
    Serial.println("tagMeeting: meeting " + String(meetingId) + " tagged '" + tag + "'");
    return true;
  }

  // UPLOAD_SERVER_URL's path with a trailing "/upload" removed, i.e. the
  // prefix the receiver's other routes hang off. "" when it's served at the
  // root, which is the usual case.
  String uploadPathPrefix() {
    String host, reqPath;
    int port;
    bool useTls;
    if (!parseUrl(host, port, reqPath, useTls)) return "";
    if (reqPath.endsWith("/upload")) return reqPath.substring(0, reqPath.length() - 7);
    return "";
  }

  // Sends [offset, offset+len) of the open file as one chunk POST. Returns
  // true whenever a well-formed HTTP response came back (including a 409
  // offset-mismatch - the caller resyncs to serverBytesReceived rather than
  // treating that as a failure); returns false when no usable response came
  // back at all (dropped connection, stalled write, or timeout).
  bool sendChunk(Session &session, const String &host, int port, const String &reqPath, const String &fileName,
                File &f, size_t offset, size_t len, bool isFinal, bool useTls, int &statusCode,
                long &serverBytesReceived, bool &complete, long &meetingId) {
    if (!session.ensureOpen(host, port, useTls)) {
      Serial.println("sendChunk: connect failed");
      return false;
    }
    WiFiClient &client = *session.client;

    String path = reqPath + "/chunk";
    // One write() for the whole header block - see the matching comment in
    // queryResumeOffset().
    String req = "POST " + path + " HTTP/1.1\r\n" +
                "Host: " + host + "\r\n" +
                "Content-Type: application/octet-stream\r\n" +
                "Content-Length: " + String((unsigned)len) + "\r\n" +
                "X-Upload-Token: " + UPLOAD_TOKEN + "\r\n" +
                "X-File-Name: " + fileName + "\r\n" +
                "X-Chunk-Offset: " + String((unsigned)offset) + "\r\n" +
                "X-Upload-Final: " + (isFinal ? "1" : "0") + "\r\n" +
                "Connection: keep-alive\r\n\r\n";
    client.write((const uint8_t *)req.c_str(), req.length());

    f.seek(offset);

    // Stall watchdog: the clock resets on every buffer actually written, so
    // it only fires if the link genuinely stops accepting data mid-chunk.
    bool streamOk = true;
    unsigned long lastProgressMs = millis();
    size_t sentBytes = 0;
    uint8_t buf[UPLOAD_STREAM_BUFFER_BYTES];
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

    if (!streamOk) {
      Serial.println("sendChunk: stream stalled/dropped at " + String((unsigned)sentBytes) + "/" +
                     String((unsigned)len) + " bytes into chunk");
      session.close();
      return false;
    }

    String body;
    bool keepAlive = false;
    bool ok = readHttpResponse(client, millis() + UPLOAD_SOCKET_TIMEOUT_MS, statusCode, body, keepAlive);
    if (!keepAlive) session.close();
    if (!ok) {
      Serial.println("sendChunk: no valid HTTP response (timeout/disconnect)");
      return false;
    }

    // Body is "<bytesReceived>", "<bytesReceived> COMPLETE", or
    // "<bytesReceived> COMPLETE <meetingId>"; toInt() reads the leading
    // digits and ignores whatever trails it in every case.
    int completeIdx = body.indexOf("COMPLETE");
    complete = completeIdx >= 0;
    serverBytesReceived = body.toInt();
    // The id is only present on the response that finalizes the file, and
    // only from a server new enough to send it - stays -1 otherwise, which
    // tagMeeting() treats as "nothing to tag".
    meetingId = -1;
    if (complete) {
      String trailer = body.substring(completeIdx + 8);
      trailer.trim();
      if (trailer.length() > 0) meetingId = trailer.toInt();
    }
    return true;
  }
};
