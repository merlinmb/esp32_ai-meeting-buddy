#pragma once
#include <Arduino.h>
#include <time.h>

// M5StickS3 has no RTC chip - wall-clock time comes from NTP over WiFi
// instead. Once synced, the ESP32's internal RTC-timer keeps counting
// through deep sleep/reset-free runtime, so getTime() stays accurate
// between syncs without needing WiFi up continuously.

struct RtcTime {
  int year;    // full year, e.g. 2026
  int month;   // 1-12
  int day;     // 1-31
  int hour;    // 0-23
  int minute;  // 0-59
  int second;  // 0-59
};

class NtpClock {
 public:
  // Call while WiFi is connected. Blocks briefly waiting for NTP.
  // tz is a POSIX TZ string so DST is applied automatically, e.g.
  // "GMT0BST,M3.5.0/1,M10.5.0" for Europe/London.
  bool sync(const char *tz = "GMT0BST,M3.5.0/1,M10.5.0",
            const char *ntpServer = "pool.ntp.org") {
    configTzTime(tz, ntpServer);
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 10000)) return false;
    _synced = true;
    return true;
  }

  bool isSynced() const { return _synced; }

  bool getTime(RtcTime &out) const {
    if (!_synced) return false;
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 0)) return false;
    out.year   = timeinfo.tm_year + 1900;
    out.month  = timeinfo.tm_mon + 1;
    out.day    = timeinfo.tm_mday;
    out.hour   = timeinfo.tm_hour;
    out.minute = timeinfo.tm_min;
    out.second = timeinfo.tm_sec;
    return true;
  }

  // Formats as MEETING_YYYYMMDD_HHMMSS for filenames.
  String filenameTimestamp() const {
    RtcTime t;
    if (!getTime(t)) return "MEETING_UNKNOWN_TIME_" + String(millis());
    char buf[32];
    snprintf(buf, sizeof(buf), "MEETING_%04d%02d%02d_%02d%02d%02d",
             t.year, t.month, t.day, t.hour, t.minute, t.second);
    return String(buf);
  }

 private:
  bool _synced = false;
};
