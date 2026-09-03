#include "Arduino.h"
#include "../../config.h"
#include "../../globals.h"
#include "../../types.h"
#include "network.h"
#include "notes.h"
#include "rtc.h"
#include "ui.h"
#include "WiFi.h"
#include "WiFiClientSecure.h"
#include <WebServer.h>
#include "SD_MMC.h"
#include "esp_heap_caps.h"
#include "../../secrets.h"

static String parseWhisperText(const String& resp) {
  int s = resp.indexOf("\"text\":\"");
  if (s < 0) return "";
  s += 8;
  int e = s;
  while (e < (int)resp.length()) {
    if (resp[e] == '\\' && e + 1 < (int)resp.length()) { e += 2; continue; }
    if (resp[e] == '"') break;
    e++;
  }
  if (e >= (int)resp.length()) return "";
  String text = "";
  for (int i = s; i < e; i++) {
    if (resp[i] == '\\' && i + 1 < e) {
      char nx = resp[++i];
      if      (nx == '"')  text += '"';
      else if (nx == '\\') text += '\\';
      else if (nx == 'n')  text += ' ';
      else                 text += nx;
    } else {
      text += resp[i];
    }
  }
  return text;
}

// ─── Notes-server upload (chunked / resumable) ─────────────────────────────
//
// Audio goes to the personal notes server (UPLOAD_SERVER_URL), which runs
// Whisper and the AI cleanup itself, so recordings stay on hardware you own
// rather than a third-party transcription API. The protocol is the server's:
// it owns the byte offset, the device never assumes one. See /upload/status
// and /upload/chunk in the server.

// UPLOAD_SERVER_URL points at the /upload endpoint; everything else on the
// server (/upload/chunk, /upload/status, /api/notes/...) hangs off the same
// origin, so the scheme, host and port are parsed out of it once and the
// paths are built from there. Keeps secrets.h the single place the server
// address is configured.
struct ServerUrl {
  String host;
  int    port;
  bool   tls;
  String base;    // path prefix the /upload endpoint sits under, usually ""
};

static const ServerUrl& serverUrl() {
  static ServerUrl u;
  static bool parsed = false;
  if (parsed) return u;
  parsed = true;

  String raw = UPLOAD_SERVER_URL;
  u.tls  = raw.startsWith("https://");
  u.port = u.tls ? 443 : 80;

  int schemeEnd = raw.indexOf("://");
  String rest = (schemeEnd >= 0) ? raw.substring(schemeEnd + 3) : raw;

  int slash = rest.indexOf('/');
  String hostPort = (slash >= 0) ? rest.substring(0, slash) : rest;
  String path     = (slash >= 0) ? rest.substring(slash)    : "";

  int colon = hostPort.indexOf(':');
  if (colon >= 0) {
    u.host = hostPort.substring(0, colon);
    u.port = hostPort.substring(colon + 1).toInt();
  } else {
    u.host = hostPort;
  }

  // Strip the trailing "/upload" so sibling paths can be appended cleanly.
  // Any trailing slash goes first, or a URL written ".../upload/" would keep
  // its "/upload" and every request would end up doubled as "/upload/upload".
  while (path.endsWith("/")) path = path.substring(0, path.length() - 1);
  if (path.endsWith("/upload")) path = path.substring(0, path.length() - 7);
  while (path.endsWith("/")) path = path.substring(0, path.length() - 1);
  u.base = path;

  Serial.printf("[Server] %s://%s:%d%s\n", u.tls ? "https" : "http",
                u.host.c_str(), u.port, u.base.c_str());
  return u;
}

// One connection per request (the server closes them anyway). TLS uses
// setInsecure: this is a personal server, often on a LAN name or a private
// CA, and pinning a cert the device can't rotate would break it silently.
static WiFiClient* openServerConn() {
  const ServerUrl& u = serverUrl();
  // setTimeout() is Stream's, which is documented in milliseconds - passing
  // seconds here left every read with a 20ms budget, so a server that took
  // even a moment to answer looked like a dead connection.
  if (u.tls) {
    WiFiClientSecure* sc = new WiFiClientSecure();
    sc->setInsecure();
    sc->setTimeout(UPLOAD_HTTP_TO_MS);
    if (!sc->connect(u.host.c_str(), u.port)) { delete sc; return nullptr; }
    return sc;
  }
  WiFiClient* pc = new WiFiClient();
  pc->setTimeout(UPLOAD_HTTP_TO_MS);
  if (!pc->connect(u.host.c_str(), u.port)) { delete pc; return nullptr; }
  return pc;
}

// Reads one plain-text response. The server answers these endpoints with a
// leading integer (bytes it has) and an optional " COMPLETE <id>" marker, so
// only the status line and the first body line are of interest.
static bool readPlainResponse(WiFiClient& client, int& httpCode, String& body) {
  httpCode = 0;
  body = "";
  uint32_t deadline = millis() + UPLOAD_HTTP_TO_MS;

  String statusLine = "";
  while (millis() < deadline) {
    if (!client.available()) {
      if (!client.connected()) break;
      delay(5);
      continue;
    }
    statusLine = client.readStringUntil('\n');
    break;
  }
  if (!statusLine.startsWith("HTTP/")) return false;
  int sp = statusLine.indexOf(' ');
  if (sp < 0) return false;
  httpCode = statusLine.substring(sp + 1, sp + 5).toInt();

  bool inBody = false;
  while (millis() < deadline) {
    if (!client.available()) {
      if (!client.connected()) break;
      delay(5);
      continue;
    }
    String line = client.readStringUntil('\n');
    if (!inBody) {
      line.trim();
      if (line.length() == 0) inBody = true;
    } else {
      body += line;
      if (body.length() > 256) break;   // responses here are tiny
    }
  }
  body.trim();
  return true;
}

// The device's filename for a note, which is also the server-side key for
// resuming: it's RTC-timestamped where possible so it stays unique, and
// falls back to the note number when the clock was never set.
//
// Every name this produces is "note_<digits>[_<digits>].wav", i.e. already
// URL-safe, which is why the ?name= query parameters built from it are sent
// without escaping. Keep it that way if this format ever changes.
static String uploadFileName(int noteNum) {
  String created = noteCreatedUtc(noteNum);
  String stamp = "";
  for (int i = 0; i < (int)created.length(); i++) {
    char c = created[i];
    if (c >= '0' && c <= '9') stamp += c;   // 2026-09-02T14:03:11Z -> 20260902140311
  }
  char buf[64];
  if (stamp.length() >= 14) snprintf(buf, sizeof(buf), "note_%03d_%s.wav", noteNum, stamp.c_str());
  else                      snprintf(buf, sizeof(buf), "note_%03d.wav", noteNum);
  return String(buf);
}

// Device tags -> the server's fixed tag vocabulary. Anything the server
// doesn't know is left to the server's own default rather than sent and
// rejected with a 400.
static String serverTagFor(const char* deviceTag) {
  if (!deviceTag) return "";
  if (strcasecmp(deviceTag, "Private") == 0) return "Personal";
  const char* known[] = { "Meeting", "Note", "Idea", "Interview", "Personal", "Work", "Buy", nullptr };
  for (int i = 0; known[i]; i++)
    if (strcasecmp(deviceTag, known[i]) == 0) return String(known[i]);
  return "";
}

// How many bytes of this file the server already holds. -1 on failure, so a
// server that can't be reached is never mistaken for "start from zero".
static long serverOffsetFor(const String& fileName) {
  const ServerUrl& u = serverUrl();
  WiFiClient* client = openServerConn();
  if (!client) return -1;

  client->printf("GET %s/upload/status?name=%s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "X-Upload-Token: %s\r\n"
                 "Connection: close\r\n\r\n",
                 u.base.c_str(), fileName.c_str(), u.host.c_str(), UPLOAD_TOKEN);

  int code; String body;
  bool ok = readPlainResponse(*client, code, body);
  client->stop();
  delete client;
  if (!ok || code != 200) {
    Serial.printf("[Upload] status query failed (code %d)\n", code);
    return -1;
  }
  return body.toInt();
}

// Sends one chunk starting at `offset`. On success `serverBytes` is what the
// server now holds. A 409 is not a failure: the server is correcting our
// offset, and `serverBytes` carries the true one to resume from.
static bool sendChunk(const String& fileName, File& f, size_t offset,
                      size_t len, bool isFinal, long& serverBytes,
                      long& meetingId, bool& complete) {
  serverBytes = -1;
  complete = false;

  const ServerUrl& u = serverUrl();
  WiFiClient* client = openServerConn();
  if (!client) return false;

  client->printf("POST %s/upload/chunk HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "X-Upload-Token: %s\r\n"
                 "X-File-Name: %s\r\n"
                 "X-Chunk-Offset: %u\r\n"
                 "X-Upload-Final: %s\r\n"
                 "Content-Type: application/octet-stream\r\n"
                 "Content-Length: %u\r\n"
                 "Connection: close\r\n\r\n",
                 u.base.c_str(), u.host.c_str(), UPLOAD_TOKEN, fileName.c_str(),
                 (unsigned)offset, isFinal ? "1" : "0", (unsigned)len);

  if (!f.seek(offset)) { client->stop(); delete client; return false; }

  uint8_t* buf = (uint8_t*)heap_caps_malloc(1024, MALLOC_CAP_8BIT);
  if (!buf) { client->stop(); delete client; return false; }
  // Stall watchdog: the clock resets on every buffer that actually goes out,
  // so it fires only when the link has genuinely stopped accepting data
  // mid-chunk rather than merely being slow. A chunk is 256 KB now, far too
  // long to sit inside a single fixed deadline.
  size_t sent = 0;
  bool writeOk = true;
  uint32_t lastProgress = millis();
  while (sent < len) {
    if (!client->connected() || (millis() - lastProgress) > UPLOAD_STALL_TO_MS) {
      writeOk = false;
      break;
    }
    size_t want = min((size_t)1024, len - sent);
    int n = f.read(buf, want);
    if (n <= 0) { writeOk = false; break; }
    if (client->write(buf, n) != (size_t)n) { writeOk = false; break; }
    sent += n;
    lastProgress = millis();
    yield();   // let WiFi/lwIP and the task watchdog run between buffers
  }
  heap_caps_free(buf);
  // Content-Length promised `len` bytes; anything short would be rejected
  // server-side anyway, so don't wait on a response we can't trust.
  if (!writeOk || sent != len) { client->stop(); delete client; return false; }

  int code; String body;
  bool ok = readPlainResponse(*client, code, body);
  client->stop();
  delete client;
  if (!ok) return false;

  serverBytes = body.toInt();
  if (code == 409) {
    // Not a failure: the server is telling us where it really is (a previous
    // ack we never saw, or a raced write). The caller resyncs to serverBytes
    // and carries on without spending a retry - a file whose offset needs
    // correcting a few times is still making progress.
    Serial.printf("[Upload] offset corrected to %ld\n", serverBytes);
    return true;
  }
  if (code != 200) {
    Serial.printf("[Upload] chunk rejected (code %d)\n", code);
    return false;
  }

  int marker = body.indexOf("COMPLETE");
  if (marker >= 0) {
    complete = true;
    meetingId = body.substring(marker + 8).toInt();   // "<bytes> COMPLETE <id>"
  }
  return true;
}

// Applies the note's tag to the finished recording. Best-effort: the audio
// is already safely uploaded, so a failure here isn't worth failing the note.
static void sendTag(long meetingId, const char* deviceTag) {
  String tag = serverTagFor(deviceTag);
  if (meetingId <= 0 || tag.length() == 0) return;

  const ServerUrl& u = serverUrl();
  WiFiClient* client = openServerConn();
  if (!client) return;

  String payload = "tag=" + tag;
  client->printf("POST %s/api/meetings/%ld/tag HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "X-Upload-Token: %s\r\n"
                 "Content-Type: application/x-www-form-urlencoded\r\n"
                 "Content-Length: %u\r\n"
                 "Connection: close\r\n\r\n%s",
                 u.base.c_str(), meetingId, u.host.c_str(), UPLOAD_TOKEN,
                 (unsigned)payload.length(), payload.c_str());

  int code; String body;
  readPlainResponse(*client, code, body);
  client->stop();
  delete client;
  if (code != 200) Serial.printf("[Upload] tag %s failed (code %d)\n", tag.c_str(), code);
}

// Finds the server's id for an already-uploaded recording by the name it was
// stored under. Used when the upload completed but the server didn't hand
// back an id, which happens whenever this attempt resumed onto a file an
// earlier attempt had already finalized.
//
// This asks /upload/resolve rather than /api/notes: the latter lists only
// recordings that have finished transcribing, so a file finalized moments
// ago - exactly the case this function exists for - is never in it. The
// reply is a bare integer, or 404 when the server has no such recording.
static long lookupMeetingId(const String& fileName) {
  const ServerUrl& u = serverUrl();
  WiFiClient* client = openServerConn();
  if (!client) return 0;

  client->printf("GET %s/upload/resolve?name=%s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "X-Upload-Token: %s\r\n"
                 "Connection: close\r\n\r\n",
                 u.base.c_str(), fileName.c_str(), u.host.c_str(), UPLOAD_TOKEN);

  int code; String body;
  bool ok = readPlainResponse(*client, code, body);
  client->stop();
  delete client;
  if (!ok || code != 200) return 0;   // 404 = server does not have it (yet)
  return body.toInt();
}

// Uploads one note's WAV to the notes server, resuming wherever the server
// says it already got to. Returns true once the server has the whole file.
//
// `reachable` (optional) is set to false only when the server could not be
// contacted at all. It stays true for a failure that happened mid-transfer
// against a server that did answer, which the caller must not treat as
// grounds for the OpenAI fallback: the recording is meant to stay on
// hardware the user owns, and a flaky link is not a reason to ship it
// elsewhere - the note simply stays pending for the next sync.
static bool uploadToServer(const String& wavPath, int noteNum, const char* deviceTag,
                           bool* reachable = nullptr) {
  if (reachable) *reachable = true;
  File f = SD_MMC.open(wavPath.c_str());
  if (!f) return false;
  size_t fileSize = f.size();
  if (fileSize == 0) { f.close(); return false; }

  String fileName = uploadFileName(noteNum);

  long offset = serverOffsetFor(fileName);
  // The only point at which "the server isn't there" is established. Past
  // here the server has answered, so any later failure is a transfer problem
  // on a reachable server - the note stays pending for the next sync rather
  // than being handed to a third party.
  if (offset < 0) { f.close(); if (reachable) *reachable = false; return false; }
  if ((size_t)offset >= fileSize) {
    // Server already has the whole file from an earlier attempt whose final
    // ack never arrived - nothing left to send, and it was queued there.
    // That earlier attempt may not have recorded the id, so recover it now,
    // otherwise this note could never be tagged or have its text fetched.
    Serial.printf("[Upload] note %d already on server\n", noteNum);
    f.close();
    if (noteMeetingId(noteNum) <= 0) {
      long id = lookupMeetingId(fileName);
      if (id > 0) { setNoteMeetingId(noteNum, id); sendTag(id, deviceTag); }
    }
    return true;
  }

  long meetingId = 0;
  bool complete = false;
  int retries = 0;

  while ((size_t)offset < fileSize) {
    size_t len = min((size_t)UPLOAD_CHUNK_BYTES, fileSize - (size_t)offset);
    bool isFinal = ((size_t)offset + len >= fileSize);

    long serverBytes = -1;
    long before = offset;
    if (sendChunk(fileName, f, offset, len, isFinal, serverBytes, meetingId, complete)) {
      offset = (serverBytes >= 0) ? serverBytes : (long)(offset + len);
      // A 200 always advances; a 409 only resyncs, and one that hands back
      // the offset we already had would otherwise spin here forever. Count
      // those as retries so a stuck server still gives up eventually.
      if (offset > before) retries = 0;
      else if (++retries > UPLOAD_MAX_RETRIES) {
        Serial.printf("[Upload] note %d stuck at %ld/%u bytes\n", noteNum, offset, (unsigned)fileSize);
        f.close();
        return false;
      }
      if (complete) break;
      continue;
    }

    if (++retries > UPLOAD_MAX_RETRIES) {
      Serial.printf("[Upload] note %d gave up at %ld/%u bytes\n", noteNum, offset, (unsigned)fileSize);
      f.close();
      return false;
    }
    // A short-write rejection (400) hands back the offset the server really
    // has; trust it over ours.
    if (serverBytes >= 0) offset = serverBytes;
    delay(1000);
  }
  f.close();

  if (!complete) return false;
  // The id is only appended when this request is the one that finalized the
  // file; a resumed upload onto an already-finalized one gets a bare
  // "COMPLETE", so fall back to looking it up by name.
  if (meetingId <= 0) meetingId = lookupMeetingId(fileName);
  if (meetingId <= 0) {
    Serial.printf("[Upload] note %d uploaded but no meeting id\n", noteNum);
    return true;   // audio is safely there; text just can't be fetched back
  }
  sendTag(meetingId, deviceTag);
  // Remember which recording this became server-side, so a later sync can
  // fetch its processed markdown once Whisper and the AI are done with it.
  setNoteMeetingId(noteNum, meetingId);
  Serial.printf("[Upload] note %d uploaded as meeting %ld\n", noteNum, meetingId);
  return true;
}

// ─── Fetching processed notes back ─────────────────────────────────────────
//
// Transcription and AI cleanup happen on the server and take a while, so the
// text for a note usually isn't ready in the same sync that uploaded it. Each
// sync therefore asks for anything still outstanding: GET /api/notes/<id>
// returns the processed markdown as plain text (no JSON parsing needed), and
// a 404 simply means "not finished yet - ask again next time".

// Writes one note's markdown to the card, streaming it straight to SD so a
// long note never has to fit in RAM as a String. Returns false if the note
// isn't ready (404) or anything went wrong.
static bool fetchNoteText(long meetingId, int noteNum) {
  const ServerUrl& u = serverUrl();
  WiFiClient* clientPtr = openServerConn();
  if (!clientPtr) return false;
  WiFiClient& client = *clientPtr;

  client.printf("GET %s/api/notes/%ld HTTP/1.1\r\n"
                "Host: %s\r\n"
                "X-Upload-Token: %s\r\n"
                "Connection: close\r\n\r\n",
                u.base.c_str(), meetingId, u.host.c_str(), UPLOAD_TOKEN);

  uint32_t deadline = millis() + UPLOAD_HTTP_TO_MS;
  int httpCode = 0;
  String statusLine = "";
  while (millis() < deadline) {
    if (!client.available()) {
      if (!client.connected()) break;
      delay(5);
      continue;
    }
    statusLine = client.readStringUntil('\n');
    break;
  }
  if (!statusLine.startsWith("HTTP/")) { client.stop(); delete clientPtr; return false; }
  int sp = statusLine.indexOf(' ');
  if (sp < 0) { client.stop(); delete clientPtr; return false; }
  httpCode = statusLine.substring(sp + 1, sp + 5).toInt();
  if (httpCode != 200) {
    client.stop();
    delete clientPtr;
    if (httpCode != 404) Serial.printf("[Fetch] meeting %ld -> code %d\n", meetingId, httpCode);
    return false;   // 404 = still being processed
  }

  // Skip headers. Content-Length is noted so a connection that drops
  // mid-body can be told apart from a complete one.
  long contentLen = -1;
  while (millis() < deadline) {
    if (!client.available()) {
      if (!client.connected()) break;
      delay(5);
      continue;
    }
    String line = client.readStringUntil('\n');
    String probe = line; probe.trim();
    if (probe.length() == 0) break;
    probe.toLowerCase();
    if (probe.startsWith("content-length:")) contentLen = probe.substring(15).toInt();
  }

  // Write to a temp file first: a half-written .txt would otherwise look
  // like a finished note and never be fetched again.
  char tmpPath[64], finalPath[64];
  snprintf(tmpPath,   sizeof(tmpPath),   "%s/note_%03d.tmp", NOTES_DIR, noteNum);
  snprintf(finalPath, sizeof(finalPath), "%s/note_%03d.txt", NOTES_DIR, noteNum);
  if (SD_MMC.exists(tmpPath)) SD_MMC.remove(tmpPath);
  File out = SD_MMC.open(tmpPath, FILE_WRITE);
  if (!out) { client.stop(); delete clientPtr; return false; }

  uint8_t buf[512];
  long written = 0;
  while (millis() < deadline) {
    int avail = client.available();
    if (avail <= 0) {
      if (!client.connected()) break;
      delay(5);
      continue;
    }
    int n = client.read(buf, min((int)sizeof(buf), avail));
    if (n <= 0) break;
    out.write(buf, n);
    written += n;
    if (contentLen >= 0 && written >= contentLen) break;
  }
  out.close();
  client.stop();
  delete clientPtr;

  bool ok = (written > 0) && (contentLen < 0 || written == contentLen);
  if (!ok) {
    Serial.printf("[Fetch] meeting %ld truncated (%ld/%ld)\n", meetingId, written, contentLen);
    SD_MMC.remove(tmpPath);
    return false;
  }
  if (SD_MMC.exists(finalPath)) SD_MMC.remove(finalPath);
  SD_MMC.rename(tmpPath, finalPath);
  Serial.printf("[Fetch] note %d text saved (%ld bytes)\n", noteNum, written);
  return true;
}

// Asks the server for the processed text of every note that has been
// uploaded but whose markdown isn't on the card yet. Returns how many
// arrived this time round.
int fetchProcessedNotes() {
  int fetched = 0;
  for (int i = 0; i < (int)noteIndex.size(); i++) {
    int num = noteIndex[i].num;
    if (noteHasLocalText(num)) continue;
    long id = noteMeetingId(num);
    if (id <= 0) {
      // Uploaded, but the id was never recorded - the upload either predates
      // this firmware or finished while the server was still transcribing,
      // so the lookup found nothing then. Try again now.
      if (!noteIndex[i].uploaded) continue;      // not uploaded at all yet
      id = lookupMeetingId(uploadFileName(num));
      if (id <= 0) continue;
      setNoteMeetingId(num, id);
    }
    showFetchingNotes(fetched);
    if (fetchNoteText(id, num)) fetched++;
  }
  return fetched;
}

static bool transcribeOnce(const String& wavPath, int noteNum) {
  File f = SD_MMC.open(wavPath.c_str());
  if (!f) return false;
  size_t fileSize = f.size();

  String bnd = "----PalaBoundary";
  String pre = "--" + bnd + "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\nwhisper-1\r\n"
               "--" + bnd + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"note.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
  String post = "\r\n--" + bnd + "--\r\n";
  size_t totalLen = pre.length() + fileSize + post.length();

  WiFiClientSecure client;
  client.setInsecure();  // TODO: pin api.openai.com cert for production use
  client.setTimeout(90);

  if (!client.connect("api.openai.com", 443)) { f.close(); return false; }

  client.printf("POST /v1/audio/transcriptions HTTP/1.1\r\n"
                "Host: api.openai.com\r\n"
                "Authorization: Bearer %s\r\n"
                "Content-Type: multipart/form-data; boundary=%s\r\n"
                "Content-Length: %u\r\n"
                "Connection: close\r\n\r\n",
                OPENAI_KEY, bnd.c_str(), (unsigned)totalLen);
  client.print(pre);

  uint8_t* chunk = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_8BIT);
  if (!chunk) { f.close(); client.stop(); return false; }
  while (f.available()) {
    int n = f.read(chunk, 4096);
    if (n <= 0) break;
    client.write(chunk, n);
  }
  heap_caps_free(chunk);
  f.close();
  client.print(post);

  uint32_t deadline = millis() + 90000;
  while (!client.available() && millis() < deadline) delay(20);

  String resp = "";
  bool inBody = false;
  while (client.available() || (client.connected() && millis() < deadline)) {
    if (!client.available()) { delay(10); continue; }
    String line = client.readStringUntil('\n');
    if (!inBody) {
      if (line == "\r" || line == "") inBody = true;
      if (line.startsWith("HTTP/") && line.indexOf(" 200 ") < 0) {
        Serial.printf("[Whisper] %s\n", line.c_str());
        client.stop(); return false;
      }
    } else {
      resp += line;
      if (resp.length() > 8192) break;
    }
  }
  client.stop();

  String text = parseWhisperText(resp);
  if (text.length() == 0) { Serial.println("[Whisper] empty response"); return false; }

  String tp = wavPath; tp.replace(".wav", ".txt");
  File tf = SD_MMC.open(tp.c_str(), FILE_WRITE);
  if (tf) { tf.print(text); tf.close(); }

  markNoteUploaded(noteNum);
  return true;
}

bool transcribe(const String& wavPath, int noteNum) {
  for (int attempt = 0; attempt < 3; attempt++) {
    if (transcribeOnce(wavPath, noteNum)) return true;
    if (attempt < 2) { Serial.printf("[Whisper] retry %d/2\n", attempt + 1); delay(3000); }
  }
  return false;
}

// Sends one note to the personal notes server, falling back to OpenAI only
// if that server can't be reached at all. The server needs time to run
// Whisper and the AI cleanup, so the text is not available yet - the note is
// marked uploaded here and its markdown is fetched by a later sync.
bool syncNote(const String& wavPath, int noteNum, const char* deviceTag) {
  bool reachable = true;
  if (uploadToServer(wavPath, noteNum, deviceTag, &reachable)) {
    markNoteUploaded(noteNum);
    return true;
  }
  if (reachable) {
    // The server answered but the transfer didn't finish (a dropped link
    // mid-upload, or a chunk that kept failing). Whatever did land is still
    // on the server as a .part and the next sync resumes from there, so
    // leave the note pending instead of sending the audio to OpenAI - the
    // fallback exists for "no server at all", not for a bad connection.
    Serial.printf("[Sync] note %d: upload incomplete, staying pending for next sync\n", noteNum);
    return false;
  }
  Serial.printf("[Sync] note %d: server unreachable, falling back to OpenAI\n", noteNum);
  return transcribe(wavPath, noteNum);
}

void transcribeAll() {
  int pending = 0;
  for (int i=0; i<(int)noteIndex.size(); i++) if(!noteIndex[i].uploaded) pending++;
  int done = 0;
  for (int i=0; i<(int)noteIndex.size(); i++) {
    if (noteIndex[i].uploaded) continue;
    showTranscribing(done, pending);
    char wp[64]; snprintf(wp, sizeof(wp), "%s/note_%03d.wav", NOTES_DIR, noteIndex[i].num);
    if (syncNote(String(wp), noteIndex[i].num, noteIndex[i].tag)) done++;
  }
  // Then collect the processed markdown for anything the server has finished
  // since - including notes uploaded during an earlier sync.
  fetchProcessedNotes();
}

// ─── Portal helpers ────────────────────────────────────────────────────────

String htmlEscape(const String& s) {
  String out = s;
  out.replace("&", "&amp;"); out.replace("<", "&lt;");
  out.replace(">", "&gt;"); out.replace("\"", "&quot;");
  return out;
}

String readSmallFile(const char* path, size_t maxLen) {
  File f = SD_MMC.open(path);
  if (!f) return "";
  String out;
  while (f.available() && out.length() < maxLen) out += (char)f.read();
  f.close();
  return out;
}

String urlDecodeSimple(String s) {
  s.replace("+", " ");
  String out = "";
  for (int i = 0; i < (int)s.length(); i++) {
    if (s[i] == '%' && i + 2 < (int)s.length()) {
      String hex = s.substring(i + 1, i + 3);
      out += (char)strtol(hex.c_str(), nullptr, 16);
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

String portalCss() {
  return String(
    "<style>"
    ":root{font-family:-apple-system,BlinkMacSystemFont,'Inter','Segoe UI',sans-serif;color:#111;background:#f3f0e9;}"
    "body{margin:0;padding:24px;background:#f3f0e9;}"
    ".wrap{max-width:780px;margin:0 auto;}"
    ".top{display:flex;align-items:flex-end;justify-content:space-between;gap:16px;margin-bottom:24px;}"
    "h1{font-size:44px;letter-spacing:-.06em;line-height:.9;margin:0;font-weight:800;}"
    ".sub{font-size:13px;text-transform:uppercase;letter-spacing:.12em;color:#6a665f;margin-top:10px;}"
    ".pill{display:inline-flex;border:1px solid #111;border-radius:999px;padding:8px 12px;font-size:13px;background:#fffaf1;}"
    ".grid{display:grid;grid-template-columns:1fr;gap:14px;}"
    ".card{background:#fffaf1;border:1.5px solid #111;border-radius:24px;padding:18px;box-shadow:4px 4px 0 #111;}"
    ".row{display:flex;justify-content:space-between;gap:16px;align-items:flex-start;}"
    ".num{font-size:13px;letter-spacing:.08em;text-transform:uppercase;color:#6a665f;margin-bottom:8px;}"
    ".date{font-size:13px;color:#6a665f;margin:-4px 0 12px;}"
    ".title{font-size:24px;line-height:1.05;letter-spacing:-.04em;font-weight:750;margin:0 0 12px;}"
    ".tag{border:1px solid #111;border-radius:999px;padding:5px 9px;font-size:12px;white-space:nowrap;background:#111;color:#fff;}"
    ".text{font-size:15px;line-height:1.45;color:#222;margin:0 0 14px;white-space:pre-wrap;}"
    ".actions{display:flex;flex-wrap:wrap;gap:8px;margin-top:14px;}"
    "a.btn{color:#111;text-decoration:none;border:1px solid #111;border-radius:999px;padding:8px 12px;background:#f3f0e9;font-size:13px;}"
    "a.btn.primary{background:#111;color:#fff;}"
    ".empty{border:1.5px dashed #111;border-radius:24px;padding:34px;text-align:center;color:#6a665f;}"
    "audio{width:100%;margin-top:8px;}"
    "@media(max-width:520px){body{padding:16px}h1{font-size:36px}.card{border-radius:20px}.title{font-size:21px}}"
    "</style>"
  );
}

// ─── Portal handlers ───────────────────────────────────────────────────────

void handlePortalRoot() {
  loadIndex();

  String filter = "All";
  if (transferServer.hasArg("tag")) filter = transferServer.arg("tag");

  String html = "<!doctype html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>Pala Portal</title>" + portalCss() + "</head><body><div class='wrap'>";

  html += "<div class='top'><div><h1>pala<br>portal</h1>"
          "<div class='sub'>local note transfer · <a href=\"/tags\" style=\"color:inherit\">tags</a></div></div>"
          "<div class='pill'>" + String((int)noteIndex.size()) + " notes</div></div>";

  html += "<div class='actions' style='margin-bottom:18px'>";
  html += "<a class='btn " + String(filter == "All" ? "primary" : "") + "' href='/'>All</a>";
  for (int t = 0; t < tagCount; t++) {
    String tag = String(tags[t]);
    html += "<a class='btn " + String(filter == tag ? "primary" : "") + "' href='/?tag=" + tag + "'>" + htmlEscape(tag) + "</a>";
  }
  html += "</div>";

  html += "<div class='actions' style='margin-bottom:24px'>";
  html += "<a class='btn primary' href='/export.txt'>Download all TXT</a>";
  if (filter != "All")
    html += "<a class='btn' href='/export.txt?tag=" + filter + "'>Download " + htmlEscape(filter) + " TXT</a>";
  html += "</div>";

  int visibleCount = 0;
  for (int i = 0; i < (int)noteIndex.size(); i++)
    if (filter == "All" || filter == String(noteIndex[i].tag)) visibleCount++;

  if (visibleCount <= 0) {
    html += "<div class='empty'>No notes for this filter.</div>";
  } else {
    html += "<div class='grid'>";
    for (int v = 0; v < (int)noteIndex.size(); v++) {
      int i = (int)noteIndex.size() - 1 - v;
      if (!(filter == "All" || filter == String(noteIndex[i].tag))) continue;
      int num = noteIndex[i].num;

      char txtPath[64], wavPath[64];
      snprintf(txtPath, sizeof(txtPath), "%s/note_%03d.txt", NOTES_DIR, num);
      snprintf(wavPath, sizeof(wavPath), "%s/note_%03d.wav", NOTES_DIR, num);

      String transcript = readSmallFile(txtPath, 1200);
      if (transcript.length() == 0)
        transcript = noteIndex[i].uploaded ? "Uploaded - still being processed." : "Not transcribed yet.";

      String title = transcript; title.replace("\n", " "); title.trim();
      if (title.length() > 58) title = title.substring(0, 58) + "...";
      if (title.length() == 0 || title == "Not transcribed yet.")
        title = String("Voice note ") + String(num);

      html += "<div class='card'>";
      html += "<div class='row'><div><div class='num'>#" + String(num) + "</div>";
      html += "<h2 class='title'>" + htmlEscape(title) + "</h2>";
      String createdUtc = noteCreatedUtc(num);
      if (createdUtc.length() > 0)
        html += "<div class='date' data-utc='" + createdUtc + "'>" + createdUtc + "</div>";
      else
        html += "<div class='date'>time not set</div>";
      html += "</div>";
      html += "<div class='tag'>" + htmlEscape(String(noteIndex[i].tag)) + "</div></div>";
      html += "<p class='text'>" + htmlEscape(transcript) + "</p>";
      if (SD_MMC.exists(wavPath))
        html += "<audio controls src='/audio?num=" + String(num) + "'></audio>";
      html += "<div class='actions'>";
      html += "<a class='btn primary' href='/txt?num=" + String(num) + "'>Download TXT</a>";
      if (SD_MMC.exists(wavPath))
        html += "<a class='btn' href='/wav?num=" + String(num) + "'>Download WAV</a>";
      html += "<a class='btn' style='margin-left:auto;color:#c0392b;border-color:#c0392b' "
              "href='/note/delete?num=" + String(num) + "' "
              "onclick=\"return confirm('Delete note #" + String(num) + "? This cannot be undone.')\">Delete</a>";
      html += "</div></div>";
    }
    html += "</div>";
  }

  html += "<script>"
          "document.querySelectorAll('[data-utc]').forEach(function(el){"
          "var d=new Date(el.dataset.utc);"
          "if(!isNaN(d)){el.textContent=d.toLocaleString([],{year:'numeric',month:'short',day:'2-digit',hour:'2-digit',minute:'2-digit'});}"
          "});"
          "</script>";
  html += "</div></body></html>";
  transferServer.send(200, "text/html", html);
}

void handlePortalJson() {
  loadIndex();
  String json = "[";
  for (int v = 0; v < (int)noteIndex.size(); v++) {
    int i = (int)noteIndex.size() - 1 - v;
    if (v > 0) json += ",";
    json += "{";
    json += "\"num\":" + String(noteIndex[i].num) + ",";
    json += "\"tag\":\"" + String(noteIndex[i].tag) + "\",";
    // Wire name kept as "hasText" for external consumers of /api/notes,
    // even though the field it reports is really "uploaded".
    json += "\"hasText\":" + String(noteIndex[i].uploaded ? "true" : "false");
    json += "}";
  }
  json += "]";
  transferServer.send(200, "application/json", json);
}

void handleExportTxt() {
  loadIndex();
  String filter = "All";
  if (transferServer.hasArg("tag")) filter = transferServer.arg("tag");

  String exportText = "Pala Note Export\nFilter: " + filter + "\n------------------------------\n\n";

  for (int v = 0; v < (int)noteIndex.size(); v++) {
    int i = (int)noteIndex.size() - 1 - v;
    if (!(filter == "All" || filter == String(noteIndex[i].tag))) continue;
    int num = noteIndex[i].num;
    char txtPath[64]; snprintf(txtPath, sizeof(txtPath), "%s/note_%03d.txt", NOTES_DIR, num);
    String transcript = readSmallFile(txtPath, 4000);
    if (transcript.length() == 0)
      transcript = noteIndex[i].uploaded ? "Uploaded - still being processed." : "Not transcribed yet.";
    exportText += "#";
    if (num < 100) exportText += "0";
    if (num < 10)  exportText += "0";
    exportText += String(num) + " · " + String(noteIndex[i].tag) + "\n";
    String createdUtc = noteCreatedUtc(num);
    if (createdUtc.length() > 0) exportText += createdUtc + "\n";
    exportText += "\n" + transcript + "\n\n------------------------------\n\n";
    if (exportText.length() > 55000) {
      exportText += "\nExport truncated on device because it became too large.\n";
      break;
    }
  }

  String filename = "pala_notes_export";
  if (filter != "All") filename += "_" + filter;
  filename += ".txt";
  transferServer.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  transferServer.send(200, "text/plain", exportText);
}

void sendFileByNum(const char* ext, const char* mime, bool attachment) {
  if (!transferServer.hasArg("num")) { transferServer.send(400, "text/plain", "Missing num"); return; }
  int num = transferServer.arg("num").toInt();
  if (num <= 0) { transferServer.send(400, "text/plain", "Invalid num"); return; }
  char path[64]; snprintf(path, sizeof(path), "%s/note_%03d.%s", NOTES_DIR, num, ext);
  File f = SD_MMC.open(path);
  if (!f) { transferServer.send(404, "text/plain", "File not found"); return; }
  if (attachment) {
    String filename = String("note_") + String(num) + "." + String(ext);
    transferServer.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  }
  transferServer.streamFile(f, mime);
  f.close();
}

void handleTagAdd() {
  if (!transferServer.hasArg("name")) {
    transferServer.sendHeader("Location", "/tags?msg=missing");
    transferServer.send(303); return;
  }
  String name = urlDecodeSimple(transferServer.arg("name"));
  bool ok = addCustomTag(name.c_str());
  transferServer.sendHeader("Location", ok ? "/tags?msg=added" : "/tags?msg=exists");
  transferServer.send(303);
}

void handleTagDelete() {
  if (!transferServer.hasArg("name")) {
    transferServer.sendHeader("Location", "/tags?msg=missing");
    transferServer.send(303); return;
  }
  String name = urlDecodeSimple(transferServer.arg("name"));
  bool hadNotes = tagHasNotes(name.c_str());
  bool ok = deleteTag(name.c_str());
  if (ok && hadNotes) transferServer.sendHeader("Location", "/tags?msg=moved");
  else                transferServer.sendHeader("Location", ok ? "/tags?msg=deleted" : "/tags?msg=protected");
  transferServer.send(303);
}

void handleTagsPage() {
  loadTags();
  loadIndex();
  activeFilter = -1;

  String html = "<!doctype html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>Pala Tags</title>"
                "<style>"
                "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;margin:0;padding:24px;background:#f3f0e9;color:#111}"
                ".wrap{max-width:720px;margin:0 auto}"
                "h1{font-size:42px;line-height:.9;letter-spacing:-.05em;margin:0 0 22px;font-weight:800}"
                ".card{background:#fffaf1;border:1.5px solid #111;border-radius:24px;padding:18px;margin:14px 0;box-shadow:4px 4px 0 #111}"
                ".row{display:flex;justify-content:space-between;align-items:center;gap:12px;border-top:1px solid #ddd;padding:12px 0}"
                ".row:first-child{border-top:0}"
                ".tag{font-size:20px;font-weight:700}"
                ".meta{font-size:13px;color:#666;margin-top:4px}"
                "input{font:inherit;padding:12px;border:1.5px solid #111;border-radius:999px;background:#fff;width:100%;box-sizing:border-box}"
                "button,.btn{font:inherit;border:1.5px solid #111;border-radius:999px;padding:10px 14px;background:#111;color:#fff;text-decoration:none;white-space:nowrap}"
                ".danger{background:#fffaf1;color:#111}"
                ".msg{border:1.5px solid #111;border-radius:18px;padding:12px 14px;background:#fff;margin:12px 0}"
                ".hint{font-size:13px;color:#666;line-height:1.4}"
                "form.add{display:flex;gap:10px}"
                "</style></head><body><div class='wrap'>";

  html += "<h1>pala<br>tags</h1>";
  html += "<a class='btn' href='/'>Back to notes</a>";

  if (transferServer.hasArg("msg")) {
    String msg = transferServer.arg("msg");
    html += "<div class='msg'>";
    if (msg == "added") html += "Tag added.";
    else if (msg == "exists")    html += "Tag already exists or cannot be added.";
    else if (msg == "deleted")   html += "Tag deleted.";
    else if (msg == "moved")     html += "Tag deleted. Existing notes were moved to Untagged.";
    else if (msg == "protected") html += "This tag cannot be deleted.";
    else html += "Please enter a tag name.";
    html += "</div>";
  }

  html += "<div class='card'><form class='add' action='/tag/add' method='get'>"
          "<input name='name' maxlength='31' placeholder='New tag name'>"
          "<button type='submit'>Add</button></form>"
          "<p class='hint'>Tags appear on the device after recording. Keep them short for the e-paper UI.</p></div>";

  html += "<div class='card'>";
  for (int i = 0; i < tagCount; i++) {
    int cnt = 0;
    for (int n = 0; n < (int)noteIndex.size(); n++)
      if (strcmp(noteIndex[n].tag, tags[i]) == 0) cnt++;
    html += "<div class='row'><div><div class='tag'>" + htmlEscape(String(tags[i])) + "</div>";
    html += "<div class='meta'>" + String(cnt) + (cnt == 1 ? " note" : " notes");
    if (cnt > 0) html += " · deleting moves them to Untagged";
    html += "</div></div>";
    if (strcasecmp(tags[i], "Untagged") != 0) {
      html += "<a class='btn danger' href='/tag/delete?name=" + htmlEscape(String(tags[i])) + "' "
              "onclick=\"return confirm('Delete this tag? Notes will not be deleted. Existing notes will move to Untagged.');\">Delete</a>";
    }
    html += "</div>";
  }
  html += "</div></div></body></html>";
  transferServer.send(200, "text/html", html);
}

void handleNoteDelete() {
  if (!transferServer.hasArg("num")) { transferServer.send(400, "text/plain", "Missing num"); return; }
  int num = transferServer.arg("num").toInt();
  if (num <= 0) { transferServer.send(400, "text/plain", "Invalid num"); return; }
  deleteNote(num);
  transferServer.sendHeader("Location", "/");
  transferServer.send(303);
}

void setupTransferServer() {
  transferServer.on("/", HTTP_GET, handlePortalRoot);
  transferServer.on("/tags", HTTP_GET, handleTagsPage);
  transferServer.on("/tag/add", HTTP_GET, handleTagAdd);
  transferServer.on("/tag/delete", HTTP_GET, handleTagDelete);
  transferServer.on("/note/delete", HTTP_GET, handleNoteDelete);
  transferServer.on("/api/notes", HTTP_GET, handlePortalJson);
  transferServer.on("/export.txt", HTTP_GET, handleExportTxt);
  transferServer.on("/txt",   HTTP_GET, [](){ sendFileByNum("txt", "text/plain", true); });
  transferServer.on("/wav",   HTTP_GET, [](){ sendFileByNum("wav", "audio/wav",  true); });
  transferServer.on("/audio", HTTP_GET, [](){ sendFileByNum("wav", "audio/wav",  false); });
  transferServer.onNotFound([](){
    transferServer.send(404, "text/plain", "Not found");
  });
}

void stopTransferMode() {
  if (transferServerActive) {
    transferServer.stop();
    transferServerActive = false;
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  transferUrl = "";
}
