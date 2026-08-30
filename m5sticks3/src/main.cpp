// AI Meeting Buddy - firmware for M5 StickS3
//
// Flow: idle screen -> press button -> record to SD as WAV, with a live
// waveform on the LCD -> press again -> stop, finalize file -> hold the
// button from idle to open a menu (playback / upload now / wifi info /
// storage / about) navigated with short press (next item) and long press
// (select). Pushbutton rotary encoder on pins 0/1/8 for additional
// navigation.
//
// Playback (from the menu): rotate the encoder to browse recordings, press
// the main button to play the highlighted one, press again to pause/resume,
// hold or shake the device (Y axis) to back out.
//
// Before this compiles/works on real hardware:
//   1. Fill in config.h with your WiFi credentials (server URL already
//      points at the savage.local Docker deployment - see server/deploy.sh).
//   2. Verify SD card wiring per the diagram (CS/MOSI/CLK/MISO on G7/G6/G5/G4).

#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <SD.h>
#include <vector>
#include <math.h>

#include "pins.h"
#include "config.h"
#include "ntp_clock.h"
#include "wav_recorder.h"
#include "wav_player.h"
#include "ui_display.h"
#include "record_button.h"
#include "wifi_uploader.h"
#include "audio_visualizer.h"
#include "rotary_encoder.h"
#include "sounds.h"

enum class State { IDLE, RECORDING, UPLOADING, MENU, INFO, PLAYBACK_LIST, PLAYING };

static State state = State::IDLE;

static NtpClock clock_;
static WavRecorder recorder;
static UiDisplay ui;
static RecordButton button;
static RotaryEncoder encoder;
static WifiUploader uploader;
static AudioVisualizer visualizer;
static WavPlayer player;

static bool sdOk = false;
// Set whenever we transition into IDLE (see goIdle()) or show a full-screen
// interstitial while already IDLE - the next idle redraw needs a full
// fillScreen() rather than its normal partial/non-clearing update, or
// whatever was on screen before (a menu, an error) stays visible
// underneath. Plain "state = State::IDLE" elsewhere in loop()'s switch
// doesn't reach the IDLE case's redraw until the following iteration, by
// which point the transition-detection based on state history has already
// missed its edge - this flag makes the "just arrived" signal explicit and
// independent of that timing.
static bool forceIdleRedraw = false;

void goIdle() {
  state = State::IDLE;
  forceIdleRedraw = true;
}
static bool screenOn = true;
static unsigned long lastActivityMs = 0;
static unsigned long recordingStartMs = 0;
static unsigned long lastIdleRedrawMs = 0;
static String lastIdleClockBuf;
static bool lastIdleWifi = false;
static bool lastIdleSdOk = false;
static int lastIdleBatteryPct = -1;
static unsigned long lastBatteryPollMs = 0;
static int cachedBatteryPct = 100;
static unsigned long lastUploadAttemptMs = 0;
static uint32_t lastRecordingSeconds = 0;

static std::vector<MenuItem> kMenuItems = {
  {"Playback", Glyph::kPlay},
  {"Upload now", Glyph::kUpload},
  {"WiFi info", Glyph::kWifi},
  {"Storage", Glyph::kDisk},  // flips to a critical glyph in refreshMenuIcons() when SD is missing
  {"About", Glyph::kInfo},
  {"Power off", Glyph::kPower},
  {"Exit", Glyph::kExit},
};
static int menuSelectedIndex = 0;
static unsigned long menuLastActivityMs = 0;

// Storage's menu glyph reflects live SD status (disk icon when mounted,
// critical X when missing/failed) rather than a static icon.
void refreshMenuIcons() {
  for (auto &item : kMenuItems) {
    if (String(item.label) == "Storage") item.glyph = sdOk ? Glyph::kDisk : Glyph::kCrit;
  }
}

static std::vector<String> playbackList;
static int playbackSelectedIndex = 0;
static unsigned long playbackLastActivityMs = 0;

// Shake-to-go-back: watches the accelerometer's Y axis (the axis along the
// stick's long/hold dimension) for a sharp swing past kShakeThreshold g,
// then requires it to settle back down before arming again so one flick
// registers as exactly one "back" instead of repeat-firing while it's still
// wobbling.
static const float kShakeThresholdG = 1.6f;
static const float kShakeResetG = 0.6f;
static bool shakeArmed = true;

bool detectShake() {
  if (!M5.Imu.update()) return false;
  auto data = M5.Imu.getImuData();
  float ay = fabsf(data.accel.y);

  if (shakeArmed && ay > kShakeThresholdG) {
    shakeArmed = false;
    return true;
  }
  if (!shakeArmed && ay < kShakeResetG) {
    shakeArmed = true;
  }
  return false;
}

// Reads the battery percentage via the PMIC (M5StickS3 has no raw ADC
// battery-sense pin - GPIO38 is the TFT backlight).
int readBatteryPercent() {
  int level = M5.Power.getBatteryLevel();
  return constrain(level, 0, 100);
}

bool hasPendingUploads() {
  return sdOk && !WavRecorder::pendingFiles().empty();
}

void startRecording() {
  if (!sdOk) {
    Sounds::error();
    ui.showStatus(Severity::kCrit, "No SD card",
                  {"Recordings can't be saved.", "", "Insert a card, then press to retry."},
                  "PRESS  retry");
    delay(1500);
    forceIdleRedraw = true;
    return;
  }
  String base = clock_.filenameTimestamp();
  if (!recorder.startNewFile(base)) {
    Sounds::error();
    ui.showStatus(Severity::kCrit, "Save failed",
                  {"Recording stopped. Last few seconds may be lost.", "", "Check SD space, then retry."},
                  "PRESS  retry");
    delay(1500);
    forceIdleRedraw = true;
    return;
  }
  visualizer.reset();
  recordingStartMs = millis();
  state = State::RECORDING;
  Sounds::recordStart();
}

void stopRecording() {
  lastRecordingSeconds = (millis() - recordingStartMs) / 1000;
  recorder.close();
  M5.Mic.end();  // release I2S/codec between recordings to save power
  Sounds::recordEnd();
  goIdle();
}

void pumpAudioToSd() {
  // Pull a chunk of PCM from M5.Mic (its background task keeps I2S fed),
  // write it to the open WAV file, and feed the same samples to the
  // visualizer so the waveform reflects what's actually being recorded.
  static int16_t buf[1024];
  static bool pending = false;
  if (!pending) {
    pending = M5.Mic.record(buf, sizeof(buf) / sizeof(buf[0]), SAMPLE_RATE_HZ);
  }
  if (pending && !M5.Mic.isRecording()) {
    pending = false;
    recorder.write((const uint8_t *)buf, sizeof(buf));
    visualizer.feed((const uint8_t *)buf, sizeof(buf));
  }
}

void tryUpload() {
  if (!sdOk) return;
  auto pending = WavRecorder::pendingFiles();
  if (pending.empty()) return;

  state = State::UPLOADING;
  ui.showUploading(0, (int)pending.size());

  if (!uploader.connect()) {
    Sounds::error();
    ui.showStatus(Severity::kWarn, "Wi-Fi offline",
                  {"Recording still saves to SD.", "", "Uploads once Wi-Fi reconnects."},
                  "PRESS  ok");
    delay(1500);
    goIdle();
    return;
  }

  uploader.uploadAllPending(pending, [](int done, int total) {
    ui.showUploading(done, total);
  });

  uploader.disconnect();
  Sounds::uploadComplete();
  goIdle();
}

void enterPlaybackList() {
  playbackList = WavPlayer::listRecordings();
  playbackSelectedIndex = 0;
  playbackLastActivityMs = millis();
  state = State::PLAYBACK_LIST;
  ui.showRecordingList(playbackList, playbackSelectedIndex);
}

void startPlayback() {
  if (playbackList.empty()) return;
  if (!player.open(playbackList[playbackSelectedIndex])) {
    Sounds::error();
    ui.showStatus(Severity::kCrit, "Can't play file",
                  {"May be corrupted or still uploading.", "", "Try another recording."},
                  "HOLD  back");
    delay(1000);
    ui.showRecordingList(playbackList, playbackSelectedIndex);
    return;
  }
  playbackLastActivityMs = millis();
  state = State::PLAYING;
  ui.showPlayback(player.path(), 0, player.totalSeconds(), false);
}

void enterMenu() {
  menuSelectedIndex = 0;
  menuLastActivityMs = millis();
  state = State::MENU;
  refreshMenuIcons();
  ui.showMenu(kMenuItems, menuSelectedIndex);
}

void showWifiInfoScreen() {
  std::vector<String> lines;
  lines.push_back(String("SSID: ") + WIFI_SSID);
  lines.push_back(WiFi.status() == WL_CONNECTED
                       ? ("IP: " + WiFi.localIP().toString())
                       : String("Not connected"));
  lines.push_back("");
  lines.push_back("Wi-Fi only powers on to upload recordings.");
  ui.showInfo("WIFI", lines, Glyph::kWifi);
}

void showStorageInfoScreen() {
  std::vector<String> lines;
  if (!sdOk) {
    lines.push_back("No SD card detected.");
  } else {
    uint64_t totalMb = SD.totalBytes() / (1024ULL * 1024ULL);
    uint64_t usedMb = SD.usedBytes() / (1024ULL * 1024ULL);
    char buf[32];
    snprintf(buf, sizeof(buf), "%llu / %llu MB used", usedMb, totalMb);
    lines.push_back(buf);

    int pendingCount = (int)WavRecorder::pendingFiles().size();
    snprintf(buf, sizeof(buf), "%d recording(s) pending upload", pendingCount);
    lines.push_back(buf);
  }
  ui.showInfo("STORAGE", lines, sdOk ? Glyph::kDisk : Glyph::kCrit);
}

void showAboutScreen() {
  std::vector<String> lines;
  lines.push_back(String("AI Meeting Buddy v") + DEVICE_VERSION);
  lines.push_back("M5 StickS3");
  lines.push_back("");
  char buf[32];
  snprintf(buf, sizeof(buf), "Battery: %d%%", readBatteryPercent());
  lines.push_back(buf);
  lines.push_back(sdOk ? "SD card: OK" : "SD card: missing");
  lines.push_back(WiFi.status() == WL_CONNECTED ? "Wi-Fi: connected" : "Wi-Fi: offline");
  ui.showInfo("ABOUT", lines, Glyph::kInfo);
}

// Executes the currently-selected menu item, then decides what state comes next.
void runSelectedMenuItem() {
  String label = kMenuItems[menuSelectedIndex].label;

  if (label == "Playback") {
    enterPlaybackList();
    return;
  }
  if (label == "Upload now") {
    tryUpload();  // sets state internally (UPLOADING -> IDLE)
    return;
  }
  if (label == "Exit") {
    goIdle();
    return;
  }
  if (label == "Power off") {
    ui.showMessage("Powering off", {"See you next", "meeting."});
    delay(500);
    M5.Power.powerOff();  // cuts power at the PMIC; button press needed to boot again
    return;
  }

  // Info screens: show, then wait for a long-press ("hold: back") to return
  // to the menu. Handled as a lightweight sub-loop so main.cpp's top-level
  // loop() doesn't need a state per info screen.
  state = State::INFO;
  if (label == "WiFi info") showWifiInfoScreen();
  else if (label == "Storage") showStorageInfoScreen();
  else if (label == "About") showAboutScreen();
}

void setup() {
  Serial.begin(115200);

  auto cfg = M5.config();
  M5.begin(cfg);
  Sounds::init();

  bool lcdOk = ui.begin();
  ui.beginBootLog(DEVICE_VERSION);
  ui.bootLog(lcdOk ? "Display ready" : "Display init failed", lcdOk ? Severity::kGood : Severity::kCrit);
  if (!lcdOk) Serial.println("LCD init failed");

  sdOk = recorder.beginSd();
  ui.bootLog(sdOk ? "SD card mounted" : "No SD card found", sdOk ? Severity::kGood : Severity::kWarn);
  if (!sdOk) Serial.println("SD init failed - check wiring/pins.h");

  button.begin();
  encoder.begin();
  ui.bootLog("Controls ready", Severity::kGood);

  // Best-effort: no RTC chip on this board, so get wall-clock time from
  // NTP if WiFi is reachable. Recording still works if this fails/times
  // out - filenameTimestamp() falls back to a millis()-based name.
  if (uploader.connect()) {
    ui.bootLog("Wi-Fi connected", Severity::kGood);
    bool synced = clock_.sync();
    ui.bootLog(synced ? "Clock synced" : "Clock sync failed", synced ? Severity::kGood : Severity::kWarn);
    uploader.disconnect();
  } else {
    ui.bootLog("Wi-Fi unavailable", Severity::kWarn);
  }

  ui.bootLog("Ready", Severity::kGood);
  Sounds::startup();
  delay(800);
  lastActivityMs = millis();
}

void loop() {
  M5.update();
  ButtonEvent ev = button.poll();
  bool menuStepBack = false;
  bool fromEncoderRotation = false;
  if (ev == ButtonEvent::NONE) {
    // Rotary encoder mirrors the physical button: right = short press (next
    // item), left = same but backward through the menu, click = long press
    // (select). In the playback list this rotation is instead used purely
    // for navigation (see State::PLAYBACK_LIST) so the main button's short
    // press is free to mean "play" there.
    switch (encoder.poll()) {
      case EncoderEvent::RIGHT:
        ev = ButtonEvent::SHORT;
        fromEncoderRotation = true;
        break;
      case EncoderEvent::LEFT:
        ev = ButtonEvent::SHORT;
        menuStepBack = true;
        fromEncoderRotation = true;
        break;
      case EncoderEvent::BUTTON:
        ev = ButtonEvent::LONG;
        break;
      case EncoderEvent::NONE:
        break;
    }
  }

  // Shake-to-go-back: a hard flick on the Y axis backs out one level, same
  // as a long-press, from any screen except IDLE/RECORDING (where a shake
  // is more likely to be incidental handling, not a deliberate gesture).
  bool shakeBack = detectShake();

  // Backlight power save: any real input wakes the screen and resets the
  // inactivity timer. In IDLE/RECORDING (the states this applies to - see
  // below) the input that woke the screen is consumed here and does NOT
  // also perform its normal action, so a pocket bump can't start/stop a
  // recording or open the menu - it takes a second, deliberate input.
  bool hadInput = (ev != ButtonEvent::NONE) || shakeBack;
  bool woke = false;
  if (hadInput) {
    lastActivityMs = millis();
    if (!screenOn) {
      screenOn = true;
      M5.Lcd.wakeup();
      woke = true;
      lastIdleRedrawMs = 0;  // force an immediate redraw instead of waiting out the 1s throttle
    }
  }
  bool blankableState = (state == State::IDLE || state == State::RECORDING);
  if (blankableState && screenOn && (millis() - lastActivityMs > SCREEN_TIMEOUT_MS)) {
    screenOn = false;
    M5.Lcd.sleep();
  }
  if (woke && blankableState) {
    ev = ButtonEvent::NONE;
    shakeBack = false;
  }

  // lastLoopState starts as PLAYING (never IDLE) specifically so the very
  // first loop() iteration after boot is treated as "just entered idle" -
  // otherwise the first idle draw uses clearFirst=false and paints over
  // whatever the boot log left on screen above its own redraw region.
  static State lastLoopState = State::PLAYING;
  bool justEnteredIdle = (state == State::IDLE && lastLoopState != State::IDLE) || forceIdleRedraw;
  forceIdleRedraw = false;
  lastLoopState = state;

  switch (state) {
    case State::IDLE: {
      if (ev == ButtonEvent::SHORT) {
        startRecording();
        break;
      }
      if (ev == ButtonEvent::LONG) {
        enterMenu();
        break;
      }
      if (screenOn && (justEnteredIdle || millis() - lastIdleRedrawMs > 1000)) {
        lastIdleRedrawMs = millis();
        RtcTime t;
        char clockBuf[16] = "--:--";
        if (clock_.getTime(t)) {
          snprintf(clockBuf, sizeof(clockBuf), "%02d:%02d", t.hour, t.minute);
        }
        float sdFreePct = 0.0f;
        if (sdOk) {
          uint64_t sdTotal = SD.totalBytes();
          uint64_t sdUsed = SD.usedBytes();
          sdFreePct = sdTotal > 0 ? 100.0f * (float)(sdTotal - sdUsed) / (float)sdTotal : 0.0f;
        }
        if (millis() - lastBatteryPollMs > 10000 || lastBatteryPollMs == 0) {
          lastBatteryPollMs = millis();
          cachedBatteryPct = readBatteryPercent();
        }
        int batteryPct = cachedBatteryPct;
        bool wifiConnected = WiFi.status() == WL_CONNECTED;
        bool clockChanged = justEnteredIdle || lastIdleClockBuf != clockBuf || lastIdleWifi != wifiConnected ||
                             lastIdleSdOk != sdOk || lastIdleBatteryPct != batteryPct;
        if (clockChanged) {
          lastIdleClockBuf = clockBuf;
          lastIdleWifi = wifiConnected;
          lastIdleSdOk = sdOk;
          lastIdleBatteryPct = batteryPct;
          ui.showIdle(clockBuf, wifiConnected, sdOk, sdFreePct, lastRecordingSeconds, batteryPct, justEnteredIdle);
        }
      }
      if (AUTO_UPLOAD_WHEN_IDLE && (millis() - lastUploadAttemptMs > UPLOAD_RETRY_INTERVAL_MS)) {
        lastUploadAttemptMs = millis();
        tryUpload();
      }
      break;
    }

    case State::RECORDING: {
      pumpAudioToSd();
      if (ev == ButtonEvent::SHORT || ev == ButtonEvent::LONG) {
        stopRecording();
        break;
      }
      static unsigned long lastRedraw = 0;
      if (screenOn && millis() - lastRedraw > 120) {  // fast enough for the waveform to feel live
        lastRedraw = millis();
        ui.showRecording((millis() - recordingStartMs) / 1000, visualizer);
      }
      break;
    }

    case State::UPLOADING:
      // handled synchronously inside tryUpload(); nothing to do here
      break;

    case State::MENU: {
      if (ev == ButtonEvent::SHORT) {
        int count = (int)kMenuItems.size();
        menuSelectedIndex = menuStepBack
                                ? (menuSelectedIndex + count - 1) % count
                                : (menuSelectedIndex + 1) % count;
        menuLastActivityMs = millis();
        ui.showMenu(kMenuItems, menuSelectedIndex);
      } else if (ev == ButtonEvent::LONG) {
        menuLastActivityMs = millis();
        runSelectedMenuItem();
      } else if (millis() - menuLastActivityMs > MENU_IDLE_TIMEOUT_MS) {
        goIdle();  // walked away / forgot about it
      }
      break;
    }

    case State::INFO: {
      // A long-press from an info screen goes back to the menu; anything
      // else (or a timeout) also bails out to keep the UI from ever feeling
      // stuck on one screen.
      if (ev == ButtonEvent::LONG || ev == ButtonEvent::SHORT) {
        enterMenu();
      } else if (millis() - menuLastActivityMs > MENU_IDLE_TIMEOUT_MS) {
        goIdle();
      }
      break;
    }

    case State::PLAYBACK_LIST: {
      if (ev == ButtonEvent::SHORT && fromEncoderRotation) {
        // Encoder rotation: move the highlight only.
        if (!playbackList.empty()) {
          int count = (int)playbackList.size();
          playbackSelectedIndex = menuStepBack
                                       ? (playbackSelectedIndex + count - 1) % count
                                       : (playbackSelectedIndex + 1) % count;
        }
        playbackLastActivityMs = millis();
        ui.showRecordingList(playbackList, playbackSelectedIndex);
      } else if (ev == ButtonEvent::SHORT) {
        // Main button click: play the highlighted item.
        playbackLastActivityMs = millis();
        startPlayback();
      } else if (ev == ButtonEvent::LONG || shakeBack) {
        enterMenu();
      } else if (millis() - playbackLastActivityMs > MENU_IDLE_TIMEOUT_MS) {
        goIdle();
      }
      break;
    }

    case State::PLAYING: {
      if (ev == ButtonEvent::SHORT) {
        // click = pause, click again = resume
        if (player.isPaused()) player.resume();
        else player.pause();
        playbackLastActivityMs = millis();
        ui.showPlayback(player.path(), player.elapsedSeconds(), player.totalSeconds(), player.isPaused());
        break;
      }
      if (ev == ButtonEvent::LONG || shakeBack) {
        player.close();
        enterPlaybackList();
        break;
      }
      player.pump();
      if (player.finished()) {
        player.close();
        enterPlaybackList();
        break;
      }
      static unsigned long lastRedraw = 0;
      if (millis() - lastRedraw > 200) {
        lastRedraw = millis();
        ui.showPlayback(player.path(), player.elapsedSeconds(), player.totalSeconds(), player.isPaused());
      }
      break;
    }
  }
}
