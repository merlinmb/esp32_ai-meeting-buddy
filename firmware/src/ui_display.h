#pragma once
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <vector>
#include "pins.h"
#include "audio_visualizer.h"

// Status/menu UI for the 240x280 ST7789. Everything's drawn with primitive
// shapes + text (no image assets), organized as a few full-screen "views" so
// each state in main.cpp maps to exactly one draw call.

struct MenuItem {
  const char *label;
};

class UiDisplay {
 public:
  bool begin() {
    _bus = new Arduino_ESP32SPI(PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCLK, PIN_LCD_MOSI, GFX_NOT_DEFINED /* MISO not used by LCD */);
    _gfx = new Arduino_ST7789(_bus, PIN_LCD_RST, 0 /* rotation */, true /* IPS */, 240, 280);
    if (!_gfx->begin()) return false;

    pinMode(PIN_LCD_BL, OUTPUT);
    digitalWrite(PIN_LCD_BL, HIGH);

    _bg      = _gfx->color565(8, 10, 14);      // near-black
    _panel   = _gfx->color565(26, 28, 34);
    _accent  = _gfx->color565(64, 200, 170);   // teal
    _danger  = _gfx->color565(230, 70, 70);    // recording red
    _text    = _gfx->color565(230, 235, 240);
    _muted   = _gfx->color565(120, 130, 140);
    _clockGreen = _gfx->color565(80, 240, 170);
    _wifiBlue   = _gfx->color565(70, 170, 255);
    _sdYellow   = _gfx->color565(250, 200, 60);
    _cardPurple = _gfx->color565(120, 100, 235);

    _gfx->fillScreen(_bg);
    return true;
  }

  // ---- Idle: big clock, wifi status, SD space ring, last recording card --
  void showIdle(const String &clockStr, bool wifiConnected, bool sdOk, float sdFreePct, uint32_t lastRecordingSeconds) {
    _gfx->fillScreen(_bg);

    // Big vibrant clock, top-left aligned like the reference watch face.
    _gfx->setTextColor(_clockGreen);
    _gfx->setTextSize(5);
    _gfx->setCursor(16, 20);
    _gfx->print(clockStr);

    // WiFi status, upper right.
    drawWifiIcon(200, 24, wifiConnected);

    // SD-card free-space ring, bottom-left (or a fault indicator if the
    // card isn't present/couldn't be mounted).
    int ringCx = 70, ringCy = 190, ringR = 46;
    if (sdOk) {
      drawProgressRing(ringCx, ringCy, ringR, sdFreePct, _sdYellow);
      char pctBuf[8];
      snprintf(pctBuf, sizeof(pctBuf), "%d%%", (int)(sdFreePct + 0.5f));
      _gfx->setTextColor(_text);
      centerTextAt(pctBuf, ringCx, ringCy - 8, 2);
      _gfx->setTextColor(_muted);
      centerTextAt("SD FREE", ringCx, ringCy + 14, 1);
    } else {
      drawProgressRing(ringCx, ringCy, ringR, 100.0f, _danger);
      _gfx->setTextColor(_danger);
      centerTextAt("NO SD", ringCx, ringCy - 8, 2);
      _gfx->setTextColor(_muted);
      centerTextAt("CARD", ringCx, ringCy + 14, 1);
    }

    // Last-recording card, bottom-right.
    int cardX = 128, cardY = 148, cardW = 96, cardH = 84;
    _gfx->fillRoundRect(cardX, cardY, cardW, cardH, 12, _panel);
    _gfx->drawRoundRect(cardX, cardY, cardW, cardH, 12, _cardPurple);
    _gfx->setTextColor(_muted);
    _gfx->setTextSize(1);
    _gfx->setCursor(cardX + 10, cardY + 10);
    _gfx->print("LAST REC");

    uint32_t m = lastRecordingSeconds / 60;
    uint32_t s = lastRecordingSeconds % 60;
    char durBuf[8];
    snprintf(durBuf, sizeof(durBuf), "%02lu:%02lu", (unsigned long)m, (unsigned long)s);
    _gfx->setTextColor(_cardPurple);
    _gfx->setTextSize(2);
    _gfx->setCursor(cardX + 10, cardY + 34);
    _gfx->print(durBuf);

    drawFooter("press: record", "hold: menu");
  }

  // ---- Recording: live waveform + elapsed time -------------------------
  void showRecording(uint32_t elapsedSeconds, const AudioVisualizer &viz) {
    beginFrame(nullptr);

    // Pulsing-looking rec dot + label (no real animation needed, just a
    // filled circle reads fine as "recording" on a small screen).
    _gfx->fillCircle(24, 24, 6, _danger);
    _gfx->setTextColor(_danger);
    _gfx->setTextSize(2);
    _gfx->setCursor(40, 16);
    _gfx->print("REC");

    uint32_t m = elapsedSeconds / 60;
    uint32_t s = elapsedSeconds % 60;
    char buf[8];
    snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)m, (unsigned long)s);
    _gfx->setTextColor(_text);
    _gfx->setTextSize(4);
    centerText(buf, 60);

    drawWaveform(viz, /*top=*/120, /*height=*/100);

    drawFooter("press: stop", "");
  }

  // ---- Upload progress ---------------------------------------------------
  void showUploading(int doneCount, int totalCount) {
    beginFrame("UPLOADING");

    _gfx->setTextColor(_text);
    _gfx->setTextSize(3);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d / %d", doneCount, totalCount);
    centerText(buf, 90);

    // Progress bar
    int barX = 20, barY = 140, barW = 200, barH = 16;
    _gfx->drawRoundRect(barX, barY, barW, barH, 4, _muted);
    if (totalCount > 0) {
      int fillW = (int)((barW - 4) * ((float)doneCount / (float)totalCount));
      _gfx->fillRoundRect(barX + 2, barY + 2, fillW, barH - 4, 3, _accent);
    }
  }

  // ---- Upload failed: lets the user retry or dismiss ---------------------
  void showUploadFailed(int failedCount, int totalCount) {
    beginFrame("UPLOAD FAILED");

    int sentCount = totalCount - failedCount;
    _gfx->setTextColor(_danger);
    _gfx->setTextSize(3);
    char buf[24];
    snprintf(buf, sizeof(buf), "%d / %d sent", sentCount, totalCount);
    centerText(buf, 90);

    _gfx->setTextColor(_muted);
    _gfx->setTextSize(2);
    centerText("Check WiFi/server", 140);

    drawFooter("press: retry", "hold: back");
  }

  // ---- Menu: scrollable single-column list, one item highlighted --------
  void showMenu(const std::vector<MenuItem> &items, int selectedIndex) {
    beginFrame("MENU");

    int y = 60;
    for (size_t i = 0; i < items.size(); i++) {
      bool sel = ((int)i == selectedIndex);
      if (sel) {
        _gfx->fillRoundRect(16, y - 6, 208, 32, 6, _panel);
        _gfx->drawRoundRect(16, y - 6, 208, 32, 6, _accent);
      }
      _gfx->setTextColor(sel ? _accent : _text);
      _gfx->setTextSize(2);
      _gfx->setCursor(28, y);
      _gfx->print(items[i].label);
      y += 40;
    }

    drawFooter("press: next", "hold: select");
  }

  void showInfo(const String &title, const std::vector<String> &lines) {
    beginFrame(title.c_str());
    int y = 70;
    _gfx->setTextSize(2);
    for (auto &line : lines) {
      _gfx->setTextColor(_text);
      _gfx->setCursor(20, y);
      _gfx->print(line);
      y += 28;
    }
    drawFooter("hold: back", "");
  }

  void showMessage(const String &msg) {
    beginFrame(nullptr);
    _gfx->setTextColor(_text);
    _gfx->setTextSize(2);
    _gfx->setCursor(10, 120);
    _gfx->print(msg);
  }

 private:
  Arduino_DataBus *_bus = nullptr;
  Arduino_GFX *_gfx = nullptr;
  uint16_t _bg, _panel, _accent, _danger, _text, _muted;
  uint16_t _clockGreen, _wifiBlue, _sdYellow, _cardPurple;

  void beginFrame(const char *headerTitle) {
    _gfx->fillScreen(_bg);
    if (headerTitle) {
      _gfx->setTextColor(_muted);
      _gfx->setTextSize(1);
      _gfx->setCursor(16, 12);
      _gfx->print(headerTitle);
      _gfx->drawFastHLine(16, 24, 208, _panel);
    }
  }

  void drawFooter(const char *left, const char *right) {
    _gfx->drawFastHLine(16, 256, 208, _panel);
    _gfx->setTextSize(1);
    _gfx->setTextColor(_muted);
    if (left && left[0]) {
      _gfx->setCursor(16, 264);
      _gfx->print(left);
    }
    if (right && right[0]) {
      _gfx->setCursor(140, 264);
      _gfx->print(right);
    }
  }

  void centerText(const String &s, int y) {
    int16_t x1, y1;
    uint16_t w, h;
    _gfx->getTextBounds(s, 0, y, &x1, &y1, &w, &h);
    int x = (240 - (int)w) / 2;
    _gfx->setCursor(x, y);
    _gfx->print(s);
  }

  void drawBatteryIcon(int x, int y, int pct) {
    _gfx->drawRoundRect(x, y, 28, 14, 3, _muted);
    _gfx->fillRect(x + 28, y + 4, 3, 6, _muted);
    int fillW = (int)(24 * constrain(pct, 0, 100) / 100.0);
    uint16_t fillColor = (pct < 20) ? _danger : _accent;
    if (fillW > 0) _gfx->fillRect(x + 2, y + 2, fillW, 10, fillColor);
  }

  void drawWifiIcon(int x, int y, bool connected) {
    uint16_t c = connected ? _wifiBlue : _muted;
    _gfx->drawCircle(x + 8, y + 10, 3, c);
    _gfx->drawCircle(x + 8, y + 10, 8, c);
    if (connected) _gfx->drawCircle(x + 8, y + 10, 12, c);
  }

  // Text centered on (cx, cy) rather than top-left at (x, y).
  void centerTextAt(const String &s, int cx, int cy, uint8_t textSize) {
    _gfx->setTextSize(textSize);
    int16_t x1, y1;
    uint16_t w, h;
    _gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
    _gfx->setCursor(cx - (int)w / 2, cy - (int)h / 2);
    _gfx->print(s);
  }

  // Circular progress ring (e.g. SD card free space), 0-100%, drawn as a
  // background track plus a foreground arc swept clockwise from the top.
  void drawProgressRing(int cx, int cy, int r, float pct, uint16_t color) {
    pct = constrain(pct, 0.0f, 100.0f);
    const int thickness = 8;
    for (int rr = r - thickness; rr <= r; rr++) {
      _gfx->drawCircle(cx, cy, rr, _panel);
    }
    float sweepDeg = 360.0f * (pct / 100.0f);
    for (float a = -90.0f; a < -90.0f + sweepDeg; a += 2.0f) {
      float rad = a * PI / 180.0f;
      for (int rr = r - thickness; rr <= r; rr++) {
        int px = cx + (int)(cosf(rad) * rr);
        int py = cy + (int)(sinf(rad) * rr);
        _gfx->drawPixel(px, py, color);
      }
    }
  }

  // Vertical bar waveform from the visualizer's ring buffer.
  void drawWaveform(const AudioVisualizer &viz, int top, int height) {
    uint8_t levels[AudioVisualizer::kBarCount];
    viz.snapshot(levels);

    int barCount = AudioVisualizer::kBarCount;
    int totalWidth = 208;
    int gap = 2;
    int barW = (totalWidth - gap * (barCount - 1)) / barCount;
    int midY = top + height / 2;

    int x = 16;
    for (int i = 0; i < barCount; i++) {
      int barH = (int)((levels[i] / 255.0) * height);
      if (barH < 2) barH = 2;
      uint16_t color = (i == barCount - 1) ? _accent : _gfx->color565(
          40 + (levels[i] / 2), 150 + (levels[i] / 6), 140);
      _gfx->fillRect(x, midY - barH / 2, barW, barH, color);
      x += barW + gap;
    }
  }
};
