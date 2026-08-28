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

    _bg      = _gfx->color565(12, 16, 22);     // near-black, slightly blue
    _panel   = _gfx->color565(24, 30, 40);
    _accent  = _gfx->color565(64, 200, 170);   // teal
    _danger  = _gfx->color565(230, 70, 70);    // recording red
    _text    = _gfx->color565(230, 235, 240);
    _muted   = _gfx->color565(120, 130, 140);

    _gfx->fillScreen(_bg);
    return true;
  }

  // ---- Idle: clock, battery, wifi, quick hints -----------------------
  void showIdle(const String &clockStr, int batteryPct, bool wifiConnected, bool hasPendingUploads) {
    beginFrame("AI MEETING BUDDY");

    _gfx->setTextColor(_text);
    _gfx->setTextSize(4);
    centerText(clockStr, 70);

    // Battery pill
    drawBatteryIcon(20, 120, batteryPct);
    _gfx->setTextSize(2);
    _gfx->setTextColor(_muted);
    _gfx->setCursor(56, 124);
    _gfx->printf("%d%%", batteryPct);

    // WiFi dot
    drawWifiIcon(170, 120, wifiConnected);

    if (hasPendingUploads) {
      _gfx->setTextColor(_accent);
      _gfx->setTextSize(2);
      centerText("recordings queued", 160);
    }

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
    uint16_t c = connected ? _accent : _muted;
    _gfx->drawCircle(x + 8, y + 10, 3, c);
    _gfx->drawCircle(x + 8, y + 10, 8, c);
    if (connected) _gfx->drawCircle(x + 8, y + 10, 12, c);
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
