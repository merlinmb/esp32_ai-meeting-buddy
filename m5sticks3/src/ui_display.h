#pragma once
#include <Arduino.h>
#include <M5Unified.h>
#include <math.h>
#include <vector>
#include "pins.h"
#include "audio_visualizer.h"

// UI for M5StickS3's 80x160 display using M5Unified/M5GFX.
//
// Status/error severity is conveyed by both color and icon shape (never
// color alone): a ring+X for critical, a triangle+! for warning, a
// ring+check for good, a ring+i for info. See showMessage()/drawIcon().

struct MenuItem {
  const char *label;
  char icon;  // 'g'=good, 'w'=warn, 'c'=crit, 'i'=info, 0=none
  MenuItem(const char *l, char i = 0) : label(l), icon(i) {}
};

enum class Severity { kInfo, kGood, kWarn, kCrit };

class UiDisplay {
 public:
  bool begin() {
    M5.Lcd.setRotation(0);  // portrait mode
    _bg = M5.Lcd.color565(10, 12, 15);
    _text = M5.Lcd.color565(232, 236, 239);
    _muted = M5.Lcd.color565(90, 98, 112);
    _accent = M5.Lcd.color565(79, 195, 247);
    _good = M5.Lcd.color565(74, 222, 128);
    _warn = M5.Lcd.color565(251, 191, 36);
    _crit = M5.Lcd.color565(248, 113, 113);
    _purple = M5.Lcd.color565(167, 139, 250);

    M5.Lcd.fillScreen(_bg);
    return true;
  }

  // Idle screen: clock, then a status list (WiFi / SD / battery / last
  // recording) as icon+label+value rows so nothing is a bare number.
  // clearFirst should be true only when switching into this screen from a
  // different one (menu/recording/etc); on routine per-minute redraws all
  // glyphs are opaque at fixed positions, so skipping fillScreen() avoids a
  // visible flash.
  void showIdle(const String &clockStr, bool wifiConnected, bool sdOk, float sdFreePct, uint32_t lastRecordingSeconds, int batteryPercent = 100, bool clearFirst = true) {
    if (clearFirst) M5.Lcd.fillScreen(_bg);

    M5.Lcd.setFont(&fonts::Font0);
    fitCenterText(clockStr, 10, _text, 20, 72);

    uint32_t m = lastRecordingSeconds / 60;
    uint32_t s = lastRecordingSeconds % 60;
    char durBuf[16];
    snprintf(durBuf, sizeof(durBuf), "%02lu:%02lu", (unsigned long)m, (unsigned long)s);
    char sdBuf[16];
    snprintf(sdBuf, sizeof(sdBuf), "%d%% free", (int)(sdFreePct + 0.5f));
    char batBuf[8];
    snprintf(batBuf, sizeof(batBuf), "%d%%", batteryPercent);

    int y = 46;
    y = drawStatusRow("Wi-Fi", wifiConnected ? "Connected" : "Offline", wifiConnected ? Severity::kGood : Severity::kWarn, y);
    y = drawStatusRow("SD card", sdOk ? sdBuf : "Missing", sdOk ? Severity::kGood : Severity::kCrit, y);
    y = drawStatusRow("Battery", batBuf, Severity::kInfo, y);
    y = drawStatusRow("Last rec", durBuf, Severity::kInfo, y);

    drawFooter("PRESS  record", "HOLD  menu");
  }

  // Recording: pulsing red dot (not just a static label) so "recording" is
  // legible as an active state, big timer, live waveform confirming audio.
  void showRecording(uint32_t elapsedSeconds, const AudioVisualizer &viz) {
    M5.Lcd.fillScreen(_bg);
    M5.Lcd.setFont(&fonts::Font0);

    // Pulse driven by elapsed wall time so it animates across redraws
    // without needing its own timer/state.
    float phase = (millis() % 1000) / 1000.0f;
    float pulse = 0.55f + 0.45f * fabsf(sinf(phase * 3.14159f));
    uint16_t dotColor = blend(_bg, _crit, pulse);
    M5.Lcd.fillCircle(12, 14, 5, dotColor);
    fitText("REC", 24, 9, _crit, 11, 50);

    char buf[8];
    snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)(elapsedSeconds / 60), (unsigned long)(elapsedSeconds % 60));
    fitCenterText(buf, 34, _text, 22, 72);

    drawWaveform(viz, 88, 34);
    fitCenterText("Listening...", 128, _muted, 7, 72);

    drawFooter("PRESS  stop");
  }

  // Upload progress: names what's happening and reassures the recording is
  // safe on SD until the upload is confirmed, instead of a bare fraction.
  void showUploading(int doneCount, int totalCount) {
    M5.Lcd.fillScreen(_bg);
    M5.Lcd.setFont(&fonts::Font0);

    drawIcon(Severity::kInfo, 40, 26);
    fitCenterText("Uploading", 40, _text, 10, 72);
    char buf[24];
    snprintf(buf, sizeof(buf), "%d of %d file%s", doneCount, totalCount, totalCount == 1 ? "" : "s");
    fitCenterText(buf, 56, _muted, 8, 72);

    int barX = 8, barY = 78, barW = 64, barH = 8;
    M5.Lcd.drawRoundRect(barX, barY, barW, barH, 3, _muted);
    if (totalCount > 0) {
      int fillW = (int)((barW - 2) * ((float)doneCount / (float)totalCount));
      M5.Lcd.fillRoundRect(barX + 1, barY + 1, fillW, barH - 2, 2, _accent);
    }

    fitCenterText("Kept on SD card", 116, _muted, 7, 72);
    fitCenterText("until confirmed", 124, _muted, 7, 72);

    drawFooter("HOLD  cancel");
  }

  // Menu: selection is a solid filled pill (not just an outline) so it's
  // unambiguous at a glance; each item gets a status icon where relevant
  // (see MenuItem::icon) instead of every row looking identical.
  void showMenu(const std::vector<MenuItem> &items, int selectedIndex) {
    M5.Lcd.fillScreen(_bg);
    M5.Lcd.setFont(&fonts::Font0);
    text("MENU", 6, 8, _muted, 7);

    int y = 24;
    for (size_t i = 0; i < items.size() && i < 6; i++) {
      bool sel = ((int)i == selectedIndex);
      uint16_t fg = sel ? _bg : _text;
      if (sel) {
        M5.Lcd.fillRoundRect(4, y - 3, 72, 18, 4, _accent);
      }
      if (items[i].icon) {
        drawIcon(iconSeverity(items[i].icon), 14, y + 6, sel ? _bg : iconColor(items[i].icon));
      }
      fitText(items[i].label, 24, y + 2, fg, 8, 50);
      y += 24;
    }

    drawFooter("PRESS  next", "HOLD  select");
  }

  void showInfo(const String &title, const std::vector<String> &lines) {
    M5.Lcd.fillScreen(_bg);
    M5.Lcd.setFont(&fonts::Font0);
    text(title, 8, 8, _muted, 7);

    int y = 25;
    for (auto &line : lines) {
      if (y > 132) break;
      fitText(line, 8, y, _text, 8, 68);
      y += 16;
    }

    text("HOLD  back", 8, 151, _muted, 7);
  }

  // List of recordings to choose for playback. Only a few names fit at
  // once on the 80px-wide screen, so this scrolls a window around the
  // selected index rather than trying to fit everything.
  void showRecordingList(const std::vector<String> &names, int selectedIndex) {
    M5.Lcd.fillScreen(_bg);
    M5.Lcd.setFont(&fonts::Font0);
    text("PLAYBACK", 6, 8, _muted, 7);

    if (names.empty()) {
      fitCenterText("No recordings", 60, _text, 8, 72);
    } else {
      const int kVisible = 5;
      int start = selectedIndex - kVisible / 2;
      if (start < 0) start = 0;
      if (start > (int)names.size() - kVisible) start = max(0, (int)names.size() - kVisible);

      int y = 24;
      for (int i = start; i < (int)names.size() && i < start + kVisible; i++) {
        bool sel = (i == selectedIndex);
        uint16_t fg = sel ? _bg : _text;
        if (sel) {
          M5.Lcd.fillRoundRect(4, y - 3, 72, 18, 4, _accent);
        }
        fitText(shortName(names[i]), 8, y + 2, fg, 8, 64);
        y += 24;
      }
    }

    drawFooter("PRESS  play", "HOLD  back");
  }

  // Playback: filename, elapsed/total, and a scrub bar composed as one
  // block so it reads as a single player rather than scattered facts.
  void showPlayback(const String &name, uint32_t elapsedSeconds, uint32_t totalSeconds, bool paused) {
    M5.Lcd.fillScreen(_bg);
    M5.Lcd.setFont(&fonts::Font0);

    drawIcon(paused ? Severity::kInfo : Severity::kGood, 14, 12);
    fitText(paused ? "PAUSED" : "PLAYING", 24, 6, paused ? _muted : _good, 9, 50);
    fitText(shortName(name), 6, 26, _text, 8, 72);

    char buf[24];
    snprintf(buf, sizeof(buf), "%02lu:%02lu / %02lu:%02lu",
             (unsigned long)(elapsedSeconds / 60), (unsigned long)(elapsedSeconds % 60),
             (unsigned long)(totalSeconds / 60), (unsigned long)(totalSeconds % 60));
    fitCenterText(buf, 46, _muted, 8, 72);

    int barX = 8, barY = 66, barW = 64, barH = 6;
    M5.Lcd.fillRoundRect(barX, barY, barW, barH, 3, M5.Lcd.color565(42, 46, 55));
    if (totalSeconds > 0) {
      int fillW = (int)(barW * ((float)elapsedSeconds / (float)totalSeconds));
      M5.Lcd.fillRoundRect(barX, barY, fillW, barH, 3, _good);
    }

    drawFooter("PRESS  pause", "HOLD  back");
  }

  // Status/error screen: icon + headline convey severity before any text is
  // read, body lines explain what happened and what to do (or that nothing
  // needs doing). Replaces the old flat showMessage() dump.
  void showStatus(Severity sev, const String &headline, const std::vector<String> &body, const String &footerPrimary = "") {
    M5.Lcd.fillScreen(_bg);
    M5.Lcd.setFont(&fonts::Font0);

    drawIcon(sev, 40, 22);
    fitCenterText(headline, 38, _text, 10, 72);

    int y = 58;
    for (auto &line : body) {
      if (y > 132) break;
      if (line.length() == 0) {
        y += 6;
        continue;
      }
      fitCenterText(line, y, _muted, 7, 72);
      y += 11;
    }

    if (footerPrimary.length()) drawFooter(footerPrimary);
  }

  // Kept for the few genuinely neutral one-off messages (startup banner)
  // that don't need severity treatment.
  void showMessage(const String &headline, const std::vector<String> &body = {}) {
    showStatus(Severity::kInfo, headline, body);
  }

 private:
  uint16_t _bg, _text, _muted, _accent, _good, _warn, _crit, _purple;

  // Strips the leading "/" and ".wav" so filenames fit the narrow screen.
  String shortName(const String &path) {
    String s = path.startsWith("/") ? path.substring(1) : path;
    if (s.endsWith(".wav")) s = s.substring(0, s.length() - 4);
    return s;
  }

  uint16_t severityColor(Severity sev) {
    switch (sev) {
      case Severity::kGood: return _good;
      case Severity::kWarn: return _warn;
      case Severity::kCrit: return _crit;
      default: return _accent;
    }
  }

  Severity iconSeverity(char c) {
    switch (c) {
      case 'g': return Severity::kGood;
      case 'w': return Severity::kWarn;
      case 'c': return Severity::kCrit;
      default: return Severity::kInfo;
    }
  }
  uint16_t iconColor(char c) { return severityColor(iconSeverity(c)); }

  // Linear-blends two RGB565 colors (used for the recording dot's pulse).
  uint16_t blend(uint16_t a, uint16_t b, float t) {
    uint8_t ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    uint8_t br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    uint8_t r = ar + (uint8_t)((br - ar) * t);
    uint8_t g = ag + (uint8_t)((bg - ag) * t);
    uint8_t bl = ab + (uint8_t)((bb - ab) * t);
    return (r << 11) | (g << 5) | bl;
  }

  void text(const String &s, int x, int y, uint16_t color, int size) {
    M5.Lcd.setTextColor(color, _bg);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setFont(&fonts::Font0);
    M5.Lcd.setTextFont(1);
    setPixelSize(size);
    M5.Lcd.setCursor(x, y);
    M5.Lcd.print(s);
  }

  // M5GFX doesn't scale a bitmap font to an arbitrary pixel size, so text
  // "size" here is emulated via setTextSize() on the base 6x8 Font0 grid:
  // size/8 rounded to the nearest supported integer scale, applied together
  // with textWidth() at that scale for fitText()'s shrink-to-fit math.
  void setPixelSize(int pxHeight) {
    int scale = max(1, (int)round(pxHeight / 8.0));
    M5.Lcd.setTextSize(scale);
  }

  // Shrinks text size until it fits maxWidth, then draws left-aligned at x.
  int fitText(const String &s, int x, int y, uint16_t color, int maxPx, int maxWidth) {
    M5.Lcd.setFont(&fonts::Font0);
    int scale = max(1, (int)round(maxPx / 8.0));
    while (scale > 1) {
      M5.Lcd.setTextSize(scale);
      if (M5.Lcd.textWidth(s) <= maxWidth) break;
      scale--;
    }
    M5.Lcd.setTextSize(scale);
    M5.Lcd.setTextColor(color, _bg);
    M5.Lcd.setCursor(x, y);
    M5.Lcd.print(s);
    return scale;
  }

  int fitCenterText(const String &s, int y, uint16_t color, int maxPx, int maxWidth) {
    M5.Lcd.setFont(&fonts::Font0);
    int scale = max(1, (int)round(maxPx / 8.0));
    while (scale > 1) {
      M5.Lcd.setTextSize(scale);
      if (M5.Lcd.textWidth(s) <= maxWidth) break;
      scale--;
    }
    M5.Lcd.setTextSize(scale);
    int w = M5.Lcd.textWidth(s);
    M5.Lcd.setTextColor(color, _bg);
    M5.Lcd.setCursor((80 - w) / 2, y);
    M5.Lcd.print(s);
    return scale;
  }

  void drawFooter(const String &primary, const String &secondary = "") {
    M5.Lcd.drawFastHLine(0, 140, 80, M5.Lcd.color565(34, 38, 46));
    text(primary, 6, 145, _muted, 7);
    if (secondary.length()) text(secondary, 6, 153, _muted, 7);
  }

  // Icon language: ring+X (critical), triangle+! (warning), ring+check
  // (good), ring+i (info) - shape carries the meaning, not just color, so
  // it still reads for colorblind users.
  void drawIcon(Severity sev, int cx, int cy, int colorOverride = -1) {
    uint16_t color = colorOverride >= 0 ? (uint16_t)colorOverride : severityColor(sev);
    switch (sev) {
      case Severity::kCrit:
        M5.Lcd.drawCircle(cx, cy, 7, color);
        M5.Lcd.drawLine(cx - 3, cy - 3, cx + 3, cy + 3, color);
        M5.Lcd.drawLine(cx + 3, cy - 3, cx - 3, cy + 3, color);
        break;
      case Severity::kWarn:
        M5.Lcd.drawTriangle(cx, cy - 7, cx + 7, cy + 6, cx - 7, cy + 6, color);
        M5.Lcd.drawFastVLine(cx, cy - 2, 3, color);
        M5.Lcd.fillCircle(cx, cy + 3, 1, color);
        break;
      case Severity::kGood:
        M5.Lcd.drawCircle(cx, cy, 7, color);
        M5.Lcd.drawLine(cx - 3, cy, cx - 1, cy + 3, color);
        M5.Lcd.drawLine(cx - 1, cy + 3, cx + 4, cy - 3, color);
        break;
      case Severity::kInfo:
      default:
        M5.Lcd.drawCircle(cx, cy, 7, color);
        M5.Lcd.fillCircle(cx, cy - 3, 1, color);
        M5.Lcd.drawFastVLine(cx, cy - 1, 4, color);
        break;
    }
  }

  // One "icon + label / value" status row for the idle screen; returns the
  // y for the next row.
  int drawStatusRow(const String &label, const String &value, Severity sev, int y) {
    drawIcon(sev, 13, y + 4);
    text(label, 24, y - 4, _muted, 6);
    fitText(value, 24, y + 3, _text, 8, 50);
    return y + 24;
  }

  void drawWaveform(const AudioVisualizer &viz, int top, int height) {
    uint8_t levels[AudioVisualizer::kBarCount];
    viz.snapshot(levels);

    int barCount = AudioVisualizer::kBarCount;
    int totalWidth = 72;
    int gap = 1;
    int barW = (totalWidth - gap * (barCount - 1)) / barCount;
    if (barW < 1) barW = 1;
    int midY = top + height / 2;

    int x = 4;
    for (int i = 0; i < barCount && x < 76; i++) {
      int barH = (int)((levels[i] / 255.0) * height);
      if (barH < 1) barH = 1;
      M5.Lcd.fillRect(x, midY - barH / 2, barW, barH, _accent);
      x += barW + gap;
    }
  }
};
