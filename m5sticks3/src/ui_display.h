#pragma once
#include <Arduino.h>
#include <M5Unified.h>
#include <math.h>
#include <vector>
#include <utility>
#include "pins.h"
#include "audio_visualizer.h"

// UI for M5StickS3's 135x240 (ST7789, portrait) display using M5Unified/M5GFX.
// (Not 80x160 - that's the older M5StickC's panel. This board's screen is
// 135 wide x 240 tall; all layout below is derived from _w/_h, read once in
// begin(), rather than hardcoded so a wrong assumption here can't silently
// crop content into a corner of the real panel again.)
//
// Text uses fixed integer scales of the base 6x8 Font0 grid (kScaleBody=1,
// kScaleLabel=2, kScaleHeadline=2, kScaleBig=4) rather than shrink-to-fit -
// a shrink-to-fit pass made body text a different, inconsistent size on
// every screen depending on how long the copy happened to be (e.g. the
// About screen rendering huge because "M5 StickS3" is short). Fixed sizes
// plus real word-wrap keep every screen visually consistent; copy is
// expected to fit within kScaleBody's line budget instead.
//
// Status/error severity is conveyed by both color and icon shape (never
// color alone): a ring+X for critical, a triangle+! for warning, a
// ring+check for good, a ring+i for info. See showMessage()/drawIcon().

// Menu icon glyphs are drawn shapes representing what the item does, not a
// generic marker - see drawGlyph(). Severity glyphs (kGood/kWarn/kCrit)
// override this with a status ring so e.g. Storage can flip to a critical X.
enum class Glyph { kNone, kPlay, kUpload, kWifi, kDisk, kInfo, kPower, kExit, kGood, kWarn, kCrit };

struct MenuItem {
  const char *label;
  Glyph glyph;
  MenuItem(const char *l, Glyph g = Glyph::kNone) : label(l), glyph(g) {}
};

enum class Severity { kInfo, kGood, kWarn, kCrit };

class UiDisplay {
 public:
  bool begin() {
    M5.Lcd.setRotation(0);  // portrait mode: 135 wide x 240 tall
    _w = M5.Lcd.width();
    _h = M5.Lcd.height();

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

    centerText(clockStr, 16, _text, kScaleBig);

    uint32_t m = lastRecordingSeconds / 60;
    uint32_t s = lastRecordingSeconds % 60;
    char durBuf[16];
    snprintf(durBuf, sizeof(durBuf), "%02lu:%02lu", (unsigned long)m, (unsigned long)s);
    char sdBuf[16];
    snprintf(sdBuf, sizeof(sdBuf), "%d%%", (int)(sdFreePct + 0.5f));
    char batBuf[8];
    snprintf(batBuf, sizeof(batBuf), "%d%%", batteryPercent);

    // Four rows spaced to fill the space between the clock and the footer,
    // rather than a hardcoded step that could overflow on a different
    // screen size - see reserveFooter().
    int top = 68, bottom = reserveFooter();
    int y = top;
    int step = (bottom - top) / 4;
    y = drawStatusRow("Wi-Fi", wifiConnected ? "Connected" : "Offline", wifiConnected ? Severity::kGood : Severity::kWarn, y, step);
    y = drawStatusRow("SD card", sdOk ? sdBuf : "Missing", sdOk ? Severity::kGood : Severity::kCrit, y, step);
    y = drawStatusRow("Battery", batBuf, Severity::kInfo, y, step);
    drawStatusRow("Last rec", durBuf, Severity::kInfo, y, step);

    drawFooter("record", false, "menu", true);
  }

  // Recording: pulsing red dot (not just a static label) so "recording" is
  // legible as an active state, big timer, live waveform confirming audio.
  // Called every ~120ms while recording, so this only ever repaints the
  // regions that actually changed (dot pulse, waveform, and the timer text
  // when the second ticks over) instead of fillScreen()-ing the whole
  // display each time - a full clear at that rate is what caused the
  // visible flicker. firstDraw draws the screen's static chrome once.
  void showRecording(uint32_t elapsedSeconds, const AudioVisualizer &viz, bool firstDraw) {
    if (firstDraw) {
      M5.Lcd.fillScreen(_bg);
      leftText("REC", 34, 14, _crit, kScaleLabel);
      centerText("Listening...", 188, _muted, kScaleBody);
      drawFooter("stop", false);
      _recLastTimerBuf = "";
    }

    // Pulse driven by elapsed wall time so it animates across redraws
    // without needing its own timer/state.
    float phase = (millis() % 1000) / 1000.0f;
    float pulse = 0.55f + 0.45f * fabsf(sinf(phase * 3.14159f));
    uint16_t dotColor = blend(_bg, _crit, pulse);
    M5.Lcd.fillCircle(18, 22, 7, dotColor);

    char buf[8];
    snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)(elapsedSeconds / 60), (unsigned long)(elapsedSeconds % 60));
    if (_recLastTimerBuf != buf) {
      _recLastTimerBuf = buf;
      M5.Lcd.fillRect(0, 40, _w, 32, _bg);  // erase old timer text before drawing the new value
      centerText(buf, 48, _text, kScaleBig);
    }

    drawWaveform(viz, 130, 50);
  }

  // Upload progress: names what's happening and reassures the recording is
  // safe on SD until the upload is confirmed, instead of a bare fraction.
  // Called once per file as uploads complete, so - like showRecording()/
  // showPlayback() - only the count text and progress fill are repainted
  // per call; static chrome (icon, title, border, reassurance text) draws
  // once via firstDraw.
  //
  // Header uses the same icon+title layout as showInfo()/showRecordingList()
  // - Glyph::kUpload, same as "Upload now" in the main menu - instead of a
  // centered generic info ring, so a sub-screen always carries the icon
  // that led you into it.
  // fileFraction is the current (not-yet-counted) file's own send progress
  // (0..1) so the bar keeps moving while a single file is still in flight,
  // instead of sitting still until the whole file completes.
  void showUploading(int doneCount, int totalCount, bool firstDraw, float fileFraction = 0.0f) {
    int barX = 14, barY = 120, barW = _w - 28, barH = 12;

    if (firstDraw) {
      M5.Lcd.fillScreen(_bg);
      drawGlyph(Glyph::kUpload, 22, 20, -1, -1, 10);
      leftText("UPLOADING", 40, 14, _muted, kScaleBody);
      M5.Lcd.drawRoundRect(barX, barY, barW, barH, 4, _muted);
      for (auto &line : wrapAtScale("Kept on SD card until confirmed", kScaleBody, _w - 16)) {
        centerText(line, 148, _muted, kScaleBody);
      }
      drawFooter("cancel", false);
      _upLastCountBuf = "";
      _upLastBarFillW = -1;
    }

    char buf[24];
    snprintf(buf, sizeof(buf), "%d of %d file%s", doneCount, totalCount, totalCount == 1 ? "" : "s");
    if (_upLastCountBuf != buf) {
      _upLastCountBuf = buf;
      M5.Lcd.fillRect(0, 84, _w, 12, _bg);  // erase old count text before drawing the new value
      centerText(buf, 88, _muted, kScaleBody);
    }

    if (totalCount > 0) {
      float overallFraction = ((float)doneCount + fileFraction) / (float)totalCount;
      int fillW = (int)((barW - 2) * overallFraction);
      if (fillW != _upLastBarFillW) {
        _upLastBarFillW = fillW;
        // Progress only ever grows here (doneCount/fileFraction are
        // monotonic within an upload run), so no need to erase the track
        // first the way the shrinkable recording waveform / playback scrub
        // bar do.
        M5.Lcd.fillRoundRect(barX + 1, barY + 1, fillW, barH - 2, 3, _accent);
      }
    }
  }

  // Menu: selection is a solid filled pill (not just an outline) so it's
  // unambiguous at a glance; each item gets a glyph representing what it
  // does (play triangle, upload arrow, wifi bars, disk, info, power, exit)
  // instead of every row carrying the same generic marker. Row spacing is
  // derived from how many items actually fit above the footer, so it can
  // never grow into the footer's nav/select hints the way a fixed step could, and every
  // passed-in item is shown (no silent cap). All labels render at the same
  // fixed kScaleBody - no per-item shrinking, so weight stays consistent,
  // and body size (not the larger label size) keeps a 7-item menu compact.
  void showMenu(const std::vector<MenuItem> &items, int selectedIndex) {
    M5.Lcd.fillScreen(_bg);
    leftText("MENU", 10, 10, _muted, kScaleBody);

    int top = 34, bottom = reserveFooter();
    size_t count = items.size();
    int step = count ? (bottom - top) / (int)count : 0;
    int rowH = min(step - 4, 26);
    if (rowH < 14) rowH = min(step - 2, step);  // degrade gracefully if a very long menu is ever passed

    int y = top;
    for (size_t i = 0; i < count; i++) {
      bool sel = ((int)i == selectedIndex);
      uint16_t fg = sel ? _bg : _text;
      uint16_t rowBg = sel ? _accent : _bg;
      if (sel) {
        M5.Lcd.fillRoundRect(6, y, _w - 12, rowH, 6, _accent);
      }
      drawGlyph(items[i].glyph, 24, y + rowH / 2, sel ? _bg : glyphColor(items[i].glyph), rowBg, 8);
      leftText(items[i].label, 40, y + rowH / 2 - 4, fg, kScaleBody, rowBg);
      y += step;
    }

    drawFooter("select", false, "next", true);
  }

  // title icon identifies the screen (wifi rings for WIFI, info ring for
  // ABOUT, etc) instead of a bare text label. Lines word-wrap at the fixed
  // kScaleBody size - never shrink - so callers should keep copy short
  // enough to fit the line budget rather than relying on auto-shrink.
  void showInfo(const String &title, const std::vector<String> &lines, Glyph icon = Glyph::kInfo) {
    M5.Lcd.fillScreen(_bg);

    drawGlyph(icon, 22, 20, -1, -1, 10);
    leftText(title, 40, 14, _muted, kScaleBody);

    int y = 44, bottom = reserveFooter();
    int textMaxWidth = _w - 24;
    for (auto &line : lines) {
      if (y > bottom - 16) break;
      if (line.length() == 0) { y += 10; continue; }
      for (auto &wrapped : wrapAtScale(line, kScaleBody, textMaxWidth)) {
        if (y > bottom - 16) break;
        leftText(wrapped, 12, y, _text, kScaleBody);
        y += 14;
      }
    }

    drawFooter("back", false);
  }

  // List of recordings to choose for playback, plus a trailing "Back" row
  // (index == names.size()) so there's a deliberate way out of this screen
  // beyond the shake gesture - same convention as the Storage/Network
  // submenus' own dedicated "Back" item. Scrolls a window around the
  // selected index rather than trying to fit an arbitrary number of names.
  // Header uses the same icon+title layout as showInfo()/showUploading(),
  // and the same Glyph::kPlay shown for "Playback" in the main menu, so a
  // sub-screen always carries the icon that led you into it.
  void showRecordingList(const std::vector<String> &names, int selectedIndex) {
    M5.Lcd.fillScreen(_bg);
    drawGlyph(Glyph::kPlay, 22, 20, -1, -1, 10);
    leftText("PLAYBACK", 40, 14, _muted, kScaleBody);

    int rowCount = (int)names.size() + 1;  // +1 for the trailing "Back" row
    const int kVisible = 6;
    int start = selectedIndex - kVisible / 2;
    if (start < 0) start = 0;
    if (start > rowCount - kVisible) start = max(0, rowCount - kVisible);

    int top = 34, bottom = reserveFooter();
    int shown = min(rowCount - start, kVisible);
    int step = shown ? (bottom - top) / shown : 0;
    int rowH = min(step - 4, 30);

    int y = top;
    for (int i = start; i < rowCount && i < start + kVisible; i++) {
      bool sel = (i == selectedIndex);
      uint16_t fg = sel ? _bg : _text;
      uint16_t rowBg = sel ? _accent : _bg;
      if (sel) {
        M5.Lcd.fillRoundRect(6, y, _w - 12, rowH, 6, _accent);
      }
      String label = (i < (int)names.size()) ? shortName(names[i]) : "Back";
      leftText(label, 14, y + rowH / 2 - 4, fg, kScaleBody, rowBg);
      y += step;
    }

    bool onBackRow = selectedIndex >= (int)names.size();
    drawFooter(onBackRow ? "back" : "play", false, "next", true);
  }

  // List of scanned WiFi networks to choose to connect to. Same
  // scroll-a-window-around-the-selection shape as showRecordingList(), with
  // a small dot prefix on rows whose password is already saved so the user
  // can tell a rescan-and-reselect will skip the password prompt.
  void showNetworkList(const std::vector<String> &ssids, int selectedIndex, const std::vector<bool> &saved) {
    M5.Lcd.fillScreen(_bg);
    drawGlyph(Glyph::kWifi, 22, 20, -1, -1, 10);
    leftText("NETWORKS", 40, 14, _muted, kScaleBody);

    if (ssids.empty()) {
      centerText("No networks found", 100, _muted, kScaleBody);
    } else {
      const int kVisible = 6;
      int start = selectedIndex - kVisible / 2;
      if (start < 0) start = 0;
      if (start > (int)ssids.size() - kVisible) start = max(0, (int)ssids.size() - kVisible);

      int top = 34, bottom = reserveFooter();
      int shown = min((int)ssids.size() - start, kVisible);
      int step = shown ? (bottom - top) / shown : 0;
      int rowH = min(step - 4, 30);

      int y = top;
      for (int i = start; i < (int)ssids.size() && i < start + kVisible; i++) {
        bool sel = (i == selectedIndex);
        uint16_t fg = sel ? _bg : _text;
        uint16_t rowBg = sel ? _accent : _bg;
        if (sel) {
          M5.Lcd.fillRoundRect(6, y, _w - 12, rowH, 6, _accent);
        }
        String label = (i < (int)saved.size() && saved[i]) ? "* " + ssids[i] : ssids[i];
        leftText(label, 14, y + rowH / 2 - 4, fg, kScaleBody, rowBg);
        y += step;
      }
    }

    drawFooter("connect", false, "next", true);
  }

  // On-screen QWERTY-ish grid keyboard for entering a WiFi password with
  // just next-cell/select-cell input (no touchscreen) - modeled on the
  // Bruce firmware's generalKeyboard(). A 2D cursor (col,row) rather than a
  // flat index because the caller (main.cpp) needs to know which physical
  // grid cell is highlighted to move it in the 4 wrap-around directions;
  // showKeyboard() itself only draws whatever cell it's told is selected -
  // all the actual cursor math lives in main.cpp next to the other list
  // navigation state, same as menuSelectedIndex/playbackSelectedIndex.
  //
  // Layout is fixed (not passed in) since only password entry needs this
  // right now - see kKeyRows below.
  static const int kKeyCols = 7;
  static const int kKeyRows = 5;

  // Special (non-character) keys, returned by keyAt() using values outside
  // the printable ASCII range so callers can switch on them alongside real
  // chars without a separate enum.
  static const char kKeyShift = 1;
  static const char kKeyBackspace = 2;
  static const char kKeySpace = 3;
  static const char kKeyOk = 4;
  static const char kKeyCancel = 5;

  // [row][col] -> {lower, upper}. '\0' cells are unused (kept only so every
  // row is the same width for simple indexing).
  char keyAt(int row, int col, bool shift) {
    static const char kLower[kKeyRows][kKeyCols] = {
        {'1', '2', '3', '4', '5', '6', '7'},
        {'q', 'w', 'e', 'r', 't', 'y', 'u'},
        {'i', 'o', 'p', 'a', 's', 'd', 'f'},
        {'g', 'h', 'j', 'k', 'l', 'z', 'x'},
        {'c', 'v', 'b', 'n', 'm', kKeyShift, kKeyBackspace},
    };
    static const char kUpper[kKeyRows][kKeyCols] = {
        {'8', '9', '0', '-', '_', '.', '@'},
        {'Q', 'W', 'E', 'R', 'T', 'Y', 'U'},
        {'I', 'O', 'P', 'A', 'S', 'D', 'F'},
        {'G', 'H', 'J', 'K', 'L', 'Z', 'X'},
        {'C', 'V', 'B', 'N', 'M', kKeyShift, kKeyBackspace},
    };
    if (row < 0 || row >= kKeyRows || col < 0 || col >= kKeyCols) return '\0';
    // Bottom-right two cells are always SPACE/OK/CANCEL regardless of
    // shift state - handled by the caller via keyLabel()/dedicated rows
    // below rather than living in the grid, to keep the grid all-letters.
    return shift ? kUpper[row][col] : kLower[row][col];
  }

  // OK/CANCEL/SPACE sit in a dedicated row below the letter grid (row
  // kKeyRows) rather than stealing letter cells, so every letter/digit stays
  // reachable at every shift state. Three cells: col 0 = SPACE, col 1 = OK,
  // col 2 = CANCEL - narrower cursor range than the letter grid's kKeyCols,
  // so main.cpp's navigation clamps col to [0,2] whenever it enters this row.
  static const int kActionRow = kKeyRows;

  // title: e.g. "PASSWORD". text: what's typed so far. mask: draw dots
  // instead of the real characters (for a password). cursorCol/cursorRow:
  // currently-highlighted cell, using kActionRow for the SPACE/OK/CANCEL row.
  void showKeyboard(const String &title, const String &text, bool mask, bool shift, int cursorCol, int cursorRow) {
    M5.Lcd.fillScreen(_bg);
    drawGlyph(Glyph::kWifi, 22, 20, -1, -1, 10);
    leftText(title, 40, 14, _muted, kScaleBody);

    String shown = mask ? String() : text;
    if (mask) for (size_t i = 0; i < text.length(); i++) shown += '*';
    if (shown.length() == 0) shown = " ";
    M5.Lcd.fillRect(0, 30, _w, 12, _bg);
    leftText(shown, 8, 30, _text, kScaleBody);

    int gridTop = 48;
    int gridBottom = _h - 46;  // leave room for the action row + footer
    int rowH = (gridBottom - gridTop) / kKeyRows;
    int colW = (_w - 8) / kKeyCols;

    for (int row = 0; row < kKeyRows; row++) {
      for (int col = 0; col < kKeyCols; col++) {
        char k = keyAt(row, col, shift);
        if (!k) continue;
        int x = 4 + col * colW;
        int y = gridTop + row * rowH;
        bool sel = (row == cursorRow && col == cursorCol);
        if (sel) M5.Lcd.fillRoundRect(x, y, colW - 2, rowH - 2, 3, _accent);
        String label = (k == kKeyShift) ? "^" : (k == kKeyBackspace) ? "<" : String(k);
        uint16_t fg = sel ? _bg : _text;
        centerTextIn(label, x, colW - 2, y + (rowH - 2) / 2 - 4, fg, sel ? _accent : -1);
      }
    }

    int actionY = gridTop + kKeyRows * rowH + 4;
    drawActionKey("SPACE", 4, actionY, colW * 3 - 2, cursorRow == kActionRow && cursorCol == 0);
    drawActionKey("OK", 4 + colW * 3, actionY, colW * 2 - 2, cursorRow == kActionRow && cursorCol == 1);
    drawActionKey("X", 4 + colW * 5, actionY, colW * 2 - 2, cursorRow == kActionRow && cursorCol == 2);

    drawFooter("select", false, "next", true);
  }

  // Playback: filename, elapsed/total, and a scrub bar composed as one
  // block so it reads as a single player rather than scattered facts.
  // Called every ~200ms while playing, so - like showRecording() - this
  // only repaints regions that actually changed (elapsed time text, scrub
  // bar fill) instead of fillScreen()-ing the whole display each time.
  // firstDraw draws the screen's static chrome once; pass it whenever name
  // or paused changes too, since those affect the static parts.
  //
  // Header uses the same icon+title layout as showInfo()/showRecordingList()
  // - Glyph::kPlay, same as "Playback" in the main menu - rather than a
  // playing/paused status ring, so a sub-screen always carries the icon
  // that led you into it; playing/paused is instead its own status line.
  void showPlayback(const String &name, uint32_t elapsedSeconds, uint32_t totalSeconds, bool paused, bool firstDraw) {
    if (firstDraw) {
      M5.Lcd.fillScreen(_bg);
      drawGlyph(Glyph::kPlay, 22, 20, -1, -1, 10);
      leftText("PLAYBACK", 40, 14, _muted, kScaleBody);
      leftText(paused ? "PAUSED" : "PLAYING", 12, 30, paused ? _muted : _good, kScaleLabel);
      leftText(shortName(name), 12, 52, _text, kScaleBody);
      drawFooter("pause", false);
      _pbLastTimerBuf = "";

      int barX = 14, barY = 104, barW = _w - 28, barH = 10;
      M5.Lcd.fillRoundRect(barX, barY, barW, barH, 5, M5.Lcd.color565(42, 46, 55));
    }

    char buf[24];
    snprintf(buf, sizeof(buf), "%02lu:%02lu / %02lu:%02lu",
             (unsigned long)(elapsedSeconds / 60), (unsigned long)(elapsedSeconds % 60),
             (unsigned long)(totalSeconds / 60), (unsigned long)(totalSeconds % 60));
    if (_pbLastTimerBuf != buf) {
      _pbLastTimerBuf = buf;
      M5.Lcd.fillRect(0, 68, _w, 12, _bg);  // erase old elapsed/total text before drawing the new value
      centerText(buf, 74, _muted, kScaleBody);
    }

    int barX = 14, barY = 104, barW = _w - 28, barH = 10;
    if (totalSeconds > 0) {
      int fillW = (int)(barW * ((float)elapsedSeconds / (float)totalSeconds));
      if (fillW != _pbLastBarFillW) {
        _pbLastBarFillW = fillW;
        // Erase the bar's track first so a fill that shrinks (seek/replay)
        // doesn't leave old fill pixels past the new, shorter edge.
        M5.Lcd.fillRoundRect(barX, barY, barW, barH, 5, M5.Lcd.color565(42, 46, 55));
        M5.Lcd.fillRoundRect(barX, barY, fillW, barH, 5, _good);
      }
    }
  }

  // Status/error screen: icon + headline convey severity before any text is
  // read. body is one or more paragraphs (separate strings), each
  // word-wrapped at the fixed kScaleBody size (never shrunk) - callers pass
  // full sentences rather than pre-breaking lines by hand.
  void showStatus(Severity sev, const String &headline, const std::vector<String> &body,
                   const String &footerPrimary = "", bool footerPrimaryIsNav = false) {
    M5.Lcd.fillScreen(_bg);

    drawIcon(sev, _w / 2, 38, -1, 16);
    int y = 66, bottom = reserveFooter();
    for (auto &line : wrapAtScale(headline, kScaleLabel, _w - 16)) {
      centerText(line, y, _text, kScaleLabel);
      y += 18;
    }
    y += 16;
    for (auto &para : body) {
      if (y > bottom - 12) break;
      if (para.length() == 0) { y += 8; continue; }
      for (auto &line : wrapAtScale(para, kScaleBody, _w - 24)) {
        if (y > bottom - 12) break;
        centerText(line, y, _muted, kScaleBody);
        y += 14;
      }
      y += 6;  // paragraph gap
    }

    if (footerPrimary.length()) drawFooter(footerPrimary, footerPrimaryIsNav);
  }

  // Kept for the few genuinely neutral one-off messages that don't need
  // severity treatment.
  void showMessage(const String &headline, const std::vector<String> &body = {}) {
    showStatus(Severity::kInfo, headline, body);
  }

  // Boot screen: a scrolling log of real setup() steps as they complete,
  // rather than one static "starting up..." label - each call appends a
  // line and redraws, so the screen reflects actual init progress (display
  // ready, SD mounted or missing, clock synced, etc).
  void beginBootLog(const String &version) {
    M5.Lcd.fillScreen(_bg);
    _bootLines.clear();
    drawBrandGlyph(_w / 2, 46);
    centerText("AI Meeting Buddy", 82, _text, kScaleBody);
    centerText("v" + version, 102, _muted, kScaleBody);
    _bootLogTop = 118;
    M5.Lcd.drawFastHLine(0, _bootLogTop, _w, M5.Lcd.color565(34, 38, 46));
  }

  void bootLog(const String &line, Severity sev = Severity::kInfo) {
    if (_bootLines.size() >= kMaxBootLines) _bootLines.erase(_bootLines.begin());
    _bootLines.push_back({line, sev});

    int bottom = reserveFooter();
    M5.Lcd.fillRect(0, _bootLogTop + 1, _w, bottom - _bootLogTop, _bg);
    int y = _bootLogTop + 10;
    for (auto &entry : _bootLines) {
      if (y > bottom - 14) break;
      drawIcon(entry.second, 20, y + 4, -1, 6);
      leftText(entry.first, 32, y, _muted, kScaleBody);
      y += 16;
    }
  }

 private:
  // Fixed text scales (multiples of Font0's 6x8 base glyph) - see the
  // top-of-file note on why these replaced shrink-to-fit sizing.
  static const int kScaleBody = 1;      // 8px: body copy, list rows, labels-as-values
  static const int kScaleLabel = 2;     // 16px: headings, menu items, PLAYING/PAUSED
  static const int kScaleHeadline = 2;  // 16px: status-screen headline
  static const int kScaleBig = 4;       // 32px: clock, REC timer

  int _w = 135, _h = 240;
  uint16_t _bg, _text, _muted, _accent, _good, _warn, _crit, _purple;
  static const size_t kMaxBootLines = 6;
  std::vector<std::pair<String, Severity>> _bootLines;
  int _bootLogTop = 118;
  String _recLastTimerBuf;  // last-drawn recording timer text; see showRecording()
  String _pbLastTimerBuf;   // last-drawn playback elapsed/total text; see showPlayback()
  int _pbLastBarFillW = -1; // last-drawn playback scrub bar fill width; see showPlayback()
  String _upLastCountBuf;   // last-drawn "N of M files" text; see showUploading()
  int _upLastBarFillW = -1; // last-drawn upload progress bar fill width; see showUploading()

  // Filenames are MEETING_YYYYMMDD_HHMMSS.wav (see NtpClock::
  // filenameTimestamp()) - parses that into a "Aug 30 14:30" display string
  // instead of showing the raw prefix/timestamp. Falls back to just
  // stripping "/" and ".wav" for anything that doesn't match (e.g. the
  // MEETING_UNKNOWN_TIME_<millis> name used when NTP wasn't synced).
  String shortName(const String &path) {
    String s = path.startsWith("/") ? path.substring(1) : path;
    if (s.endsWith(".wav")) s = s.substring(0, s.length() - 4);

    static const char *kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (s.startsWith("MEETING_") && s.length() == 8 + 8 + 1 + 6 &&
        s[16] == '_') {
      String datePart = s.substring(8, 16);   // YYYYMMDD
      String timePart = s.substring(17, 23);  // HHMMSS
      bool allDigits = true;
      for (size_t i = 0; i < datePart.length() && allDigits; i++) allDigits = isDigit(datePart[i]);
      for (size_t i = 0; i < timePart.length() && allDigits; i++) allDigits = isDigit(timePart[i]);
      if (allDigits) {
        int month = datePart.substring(4, 6).toInt();
        int day = datePart.substring(6, 8).toInt();
        if (month >= 1 && month <= 12) {
          char buf[16];
          snprintf(buf, sizeof(buf), "%s %d %.2s:%.2s", kMonths[month - 1], day,
                   timePart.c_str(), timePart.c_str() + 2);
          return String(buf);
        }
      }
    }
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

  uint16_t glyphColor(Glyph g) {
    switch (g) {
      case Glyph::kGood: return _good;
      case Glyph::kWarn: return _warn;
      case Glyph::kCrit: return _crit;
      case Glyph::kPower: return _crit;
      default: return _accent;
    }
  }

  // Menu-item glyphs: each shape depicts what the action does (play
  // triangle, upload arrow, wifi rings, disk, info "i", power button, exit
  // arrow) so items are recognizable without reading every label. Severity
  // glyphs reuse the ring-based status icons from drawIcon(). r is the base
  // radius (default 11); every shape's dimensions scale off it as a
  // fraction, so passing a smaller r shrinks the whole glyph consistently
  // instead of each shape needing its own hardcoded size.
  void drawGlyph(Glyph g, int cx, int cy, int colorOverride = -1, int eraseColorOverride = -1, int r = 11) {
    uint16_t color = colorOverride >= 0 ? (uint16_t)colorOverride : glyphColor(g);
    uint16_t eraseColor = eraseColorOverride >= 0 ? (uint16_t)eraseColorOverride : _bg;
    switch (g) {
      case Glyph::kGood: drawIcon(Severity::kGood, cx, cy, color, r); return;
      case Glyph::kWarn: drawIcon(Severity::kWarn, cx, cy, color, r); return;
      case Glyph::kCrit: drawIcon(Severity::kCrit, cx, cy, color, r); return;
      case Glyph::kInfo: drawIcon(Severity::kInfo, cx, cy, color, r); return;
      case Glyph::kNone: return;
      case Glyph::kPlay:
        M5.Lcd.fillTriangle(cx - r * 6 / 11, cy - r, cx - r * 6 / 11, cy + r, cx + r, cy, color);
        return;
      case Glyph::kUpload: {
        int aw = r * 8 / 11, sw = r * 6 / 11, sh = r * 8 / 11;
        M5.Lcd.fillTriangle(cx, cy - r, cx - aw, cy, cx + aw, cy, color);
        M5.Lcd.fillRect(cx - sw / 2, cy, sw, sh, color);
        M5.Lcd.drawFastHLine(cx - r, cy + r, r * 2, color);
        return;
      }
      case Glyph::kWifi:
        M5.Lcd.drawCircle(cx, cy + r * 4 / 11, r, color);
        M5.Lcd.drawCircle(cx, cy + r * 4 / 11, r * 6 / 11, color);
        M5.Lcd.fillCircle(cx, cy + r * 4 / 11, max(1, r * 2 / 11), color);
        return;
      case Glyph::kDisk: {
        int hw = r * 9 / 11, hh = r * 11 / 11;
        M5.Lcd.drawRoundRect(cx - hw, cy - hh, hw * 2, hh * 2, max(1, r * 3 / 11), color);
        M5.Lcd.drawFastHLine(cx - hw, cy - hh + hh * 8 / 11, hw * 2, color);
        M5.Lcd.fillRect(cx - hw * 4 / 9, cy - hh + hh * 3 / 11, hw * 8 / 9, max(1, r * 3 / 11), color);
        return;
      }
      case Glyph::kPower:
        M5.Lcd.drawCircle(cx, cy + r * 2 / 11, r, color);
        M5.Lcd.fillRect(cx - max(1, r * 2 / 11), cy - r, max(2, r * 3 / 11), max(2, r * 4 / 11), eraseColor);  // notch: cuts the ring's top gap
        M5.Lcd.drawFastVLine(cx, cy - r, r * 8 / 11, color);
        return;
      case Glyph::kExit:
        // A door-out arrow: vertical bar (frame) + arrow pointing right,
        // reads clearly as "leave this menu" distinct from Power's button.
        M5.Lcd.drawFastVLine(cx - r * 6 / 11, cy - r * 8 / 11, r * 16 / 11, color);
        M5.Lcd.drawFastHLine(cx - r * 6 / 11, cy - r * 8 / 11, r * 4 / 11, color);
        M5.Lcd.drawFastHLine(cx - r * 6 / 11, cy + r * 8 / 11, r * 4 / 11, color);
        M5.Lcd.drawLine(cx - r * 2 / 11, cy, cx + r, cy, color);
        M5.Lcd.drawLine(cx + r * 4 / 11, cy - r * 5 / 11, cx + r, cy, color);
        M5.Lcd.drawLine(cx + r * 4 / 11, cy + r * 5 / 11, cx + r, cy, color);
        return;
    }
  }

  // Linear-blends two RGB565 colors (used for the recording dot's pulse).
  uint16_t blend(uint16_t a, uint16_t b, float t) {
    uint8_t ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    uint8_t br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    uint8_t r = ar + (uint8_t)((br - ar) * t);
    uint8_t g = ag + (uint8_t)((bg - ag) * t);
    uint8_t bl = ab + (uint8_t)((bb - ab) * t);
    return (r << 11) | (g << 5) | bl;
  }

  // Draws s left-aligned at (x,y) at a fixed scale - no shrinking. bg lets
  // callers match the opaque text background to whatever's actually behind
  // it (e.g. a selection pill) instead of always punching a _bg-colored
  // hole through non-_bg surfaces.
  void leftText(const String &s, int x, int y, uint16_t color, int scale, int bg = -1) {
    M5.Lcd.setFont(&fonts::Font0);
    M5.Lcd.setTextSize(scale);
    M5.Lcd.setTextColor(color, bg >= 0 ? (uint16_t)bg : _bg);
    M5.Lcd.setTextWrap(false);  // clip long single-line text instead of wrapping into the row below
    M5.Lcd.setCursor(x, y);
    M5.Lcd.print(s);
  }

  // Draws s horizontally centered at y at a fixed scale - no shrinking.
  void centerText(const String &s, int y, uint16_t color, int scale, int bg = -1) {
    M5.Lcd.setFont(&fonts::Font0);
    M5.Lcd.setTextSize(scale);
    int w = M5.Lcd.textWidth(s);
    M5.Lcd.setTextColor(color, bg >= 0 ? (uint16_t)bg : _bg);
    M5.Lcd.setTextWrap(false);  // clip long single-line text instead of wrapping into the row below
    M5.Lcd.setCursor((_w - w) / 2, y);
    M5.Lcd.print(s);
  }

  // Draws s horizontally centered within [x, x+w) at y - used for keyboard
  // grid cells, which are laid out in fixed-width columns rather than
  // full-screen-centered like centerText().
  void centerTextIn(const String &s, int x, int w, int y, uint16_t color, int bg = -1) {
    M5.Lcd.setFont(&fonts::Font0);
    M5.Lcd.setTextSize(kScaleBody);
    int textW = M5.Lcd.textWidth(s);
    M5.Lcd.setTextColor(color, bg >= 0 ? (uint16_t)bg : _bg);
    M5.Lcd.setTextWrap(false);
    M5.Lcd.setCursor(x + (w - textW) / 2, y);
    M5.Lcd.print(s);
  }

  // One wide key in the keyboard's action row (SPACE/OK/CANCEL).
  void drawActionKey(const String &label, int x, int y, int w, bool sel) {
    int h = 16;
    uint16_t bg = sel ? _accent : M5.Lcd.color565(28, 31, 37);
    uint16_t fg = sel ? _bg : _muted;
    M5.Lcd.fillRoundRect(x, y, w, h, 3, bg);
    centerTextIn(label, x, w, y + h / 2 - 4, fg, bg);
  }

  // Footer is always bottom-anchored near the bottom of the screen,
  // regardless of how much content sits above it.
  int footerTop() { return _h - 34; }
  int footerLine1() { return _h - 26; }
  int footerLine2() { return _h - 14; }

  // Wraps s into lines that fit maxWidth at the given fixed scale, breaking
  // only on spaces - never mid-word. If a single word is wider than
  // maxWidth even alone, it's placed on its own line rather than cut (rare
  // at kScaleBody on a 135px-wide screen; copy should be kept short).
  std::vector<String> wrapAtScale(const String &s, int scale, int maxWidth) {
    M5.Lcd.setFont(&fonts::Font0);
    M5.Lcd.setTextSize(scale);
    std::vector<String> lines;
    int start = 0;
    while (start < (int)s.length()) {
      int end = s.length();
      int lastSpace = -1;
      for (int i = start; i < (int)s.length(); i++) {
        if (s[i] == ' ') lastSpace = i;
        String candidate = s.substring(start, i + 1);
        if (M5.Lcd.textWidth(candidate) > maxWidth) {
          end = (lastSpace > start) ? lastSpace : i;
          break;
        }
      }
      lines.push_back(s.substring(start, end));
      start = end;
      while (start < (int)s.length() && s[start] == ' ') start++;
    }
    if (lines.empty()) lines.push_back(s);
    return lines;
  }

  // Footer hints point back at the physical button that triggers them: G12
  // (nav) sits to the right of the screen, so its hint is a right-pointing
  // triangle glued to the right edge; G11 (select) is a top button pressed
  // downward, so its hint is a down-pointing triangle centered on the
  // screen. Each triangle sits immediately before its own label so the
  // glyph and text read as one unit.
  void drawNavHint(const String &label, int y) {
    M5.Lcd.setFont(&fonts::Font0);
    M5.Lcd.setTextSize(kScaleBody);
    int textW = M5.Lcd.textWidth(label);
    const int kTriSize = 6;
    const int kGap = 4;
    int rightEdge = _w - 8;
    int textX = rightEdge - textW;
    int triX = textX - kGap - kTriSize;
    int cy = y + 4;  // vertically center on the text's cap height
    M5.Lcd.fillTriangle(triX, cy - kTriSize, triX, cy + kTriSize, triX + kTriSize, cy, _muted);
    leftText(label, textX, y, _muted, kScaleBody);
  }

  void drawSelectHint(const String &label, int y) {
    M5.Lcd.setFont(&fonts::Font0);
    M5.Lcd.setTextSize(kScaleBody);
    int textW = M5.Lcd.textWidth(label);
    const int kTriSize = 6;
    const int kGap = 4;
    int unitW = kTriSize * 2 + kGap + textW;
    int triCx = (_w - unitW) / 2 + kTriSize;
    int textX = triCx + kTriSize + kGap;
    int cy = y + 4;
    M5.Lcd.fillTriangle(triCx - kTriSize, cy - kTriSize, triCx + kTriSize, cy - kTriSize, triCx, cy + kTriSize, _muted);
    leftText(label, textX, y, _muted, kScaleBody);
  }

  // primaryIsNav/secondaryIsNav pick which hint style each line uses -
  // true for a G12/nav action (right-pointing triangle, right-aligned),
  // false for a G11/select action (down-pointing triangle, centered).
  void drawFooter(const String &primary, bool primaryIsNav, const String &secondary = "", bool secondaryIsNav = false) {
    M5.Lcd.drawFastHLine(0, footerTop(), _w, M5.Lcd.color565(34, 38, 46));
    if (primary.length()) {
      if (primaryIsNav) drawNavHint(primary, footerLine1());
      else drawSelectHint(primary, footerLine1());
    }
    if (secondary.length()) {
      if (secondaryIsNav) drawNavHint(secondary, footerLine2());
      else drawSelectHint(secondary, footerLine2());
    }
  }

  // Returns the y coordinate above which content must stay clear of the
  // footer - callers use this instead of hardcoding a break point so
  // variable-length content (menus, lists) never overlaps the footer hints.
  int reserveFooter() { return footerTop() - 6; }

  // Brand mark: a small assistant/robot head (antenna, rounded face,
  // two eyes) used on the startup screen instead of a generic status
  // icon - this is identity, not a status glyph.
  void drawBrandGlyph(int cx, int cy) {
    M5.Lcd.drawFastVLine(cx, cy - 22, 6, _accent);
    M5.Lcd.fillCircle(cx, cy - 24, 2, _accent);
    M5.Lcd.drawRoundRect(cx - 16, cy - 16, 32, 26, 7, _accent);
    M5.Lcd.fillCircle(cx - 7, cy - 4, 2, _accent);
    M5.Lcd.fillCircle(cx + 7, cy - 4, 2, _accent);
    M5.Lcd.drawFastHLine(cx - 5, cy + 4, 10, _accent);
  }

  // Icon language: ring+X (critical), triangle+! (warning), ring+check
  // (good), ring+i (info) - shape carries the meaning, not just color, so
  // it still reads for colorblind users. r is the ring radius (default 11,
  // scaled for the real 135px-wide screen; smaller contexts like menu rows
  // pass a smaller r).
  void drawIcon(Severity sev, int cx, int cy, int colorOverride = -1, int r = 11) {
    uint16_t color = colorOverride >= 0 ? (uint16_t)colorOverride : severityColor(sev);
    int d = max(1, r / 2);       // half-diagonal for the X / check strokes
    int t = max(1, r * 3 / 11);  // small accent stroke length (dot stem etc)
    switch (sev) {
      case Severity::kCrit:
        M5.Lcd.drawCircle(cx, cy, r, color);
        M5.Lcd.drawLine(cx - d, cy - d, cx + d, cy + d, color);
        M5.Lcd.drawLine(cx + d, cy - d, cx - d, cy + d, color);
        break;
      case Severity::kWarn:
        M5.Lcd.drawTriangle(cx, cy - r, cx + r, cy + d, cx - r, cy + d, color);
        M5.Lcd.drawFastVLine(cx, cy - t, t + 1, color);
        M5.Lcd.fillCircle(cx, cy + t + 2, max(1, t / 2), color);
        break;
      case Severity::kGood:
        M5.Lcd.drawCircle(cx, cy, r, color);
        M5.Lcd.drawLine(cx - d, cy, cx - t, cy + d - t, color);
        M5.Lcd.drawLine(cx - t, cy + d - t, cx + d, cy - d + t, color);
        break;
      case Severity::kInfo:
      default:
        M5.Lcd.drawCircle(cx, cy, r, color);
        M5.Lcd.fillCircle(cx, cy - d + t, max(1, t / 2), color);
        M5.Lcd.drawFastVLine(cx, cy - t + 1, d, color);
        break;
    }
  }

  // One "icon + label / value" status row for the idle screen; returns the
  // y for the next row, spaced by the caller-supplied step so rows always
  // fill the space available between fixed anchors rather than a hardcoded
  // step that can overflow when the layout changes.
  int drawStatusRow(const String &label, const String &value, Severity sev, int y, int step) {
    drawIcon(sev, 22, y + 8, -1, 10);
    leftText(label, 40, y - 2, _muted, kScaleBody);
    leftText(value, 40, y + 8, _text, kScaleLabel);
    return y + step;
  }

  void drawWaveform(const AudioVisualizer &viz, int top, int height) {
    uint8_t levels[AudioVisualizer::kBarCount];
    viz.snapshot(levels);

    int barCount = AudioVisualizer::kBarCount;
    int totalWidth = _w - 16;
    int gap = 2;
    int barW = (totalWidth - gap * (barCount - 1)) / barCount;
    if (barW < 1) barW = 1;
    int midY = top + height / 2;

    int x = 8;
    for (int i = 0; i < barCount && x < _w - 8; i++) {
      int barH = (int)((levels[i] / 255.0) * height);
      if (barH < 1) barH = 1;
      // Erase the bar's full-height column first so a shrinking bar (vs.
      // last frame) doesn't leave old pixels above/below the new, shorter one.
      M5.Lcd.fillRect(x, top, barW, height, _bg);
      M5.Lcd.fillRect(x, midY - barH / 2, barW, barH, _accent);
      x += barW + gap;
    }
  }
};
