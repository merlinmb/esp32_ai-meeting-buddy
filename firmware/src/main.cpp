// AI Meeting Buddy - firmware for the Waveshare ESP32-C6-LCD-1.69
//
// Flow: idle screen -> press button -> record to SD as WAV, with a live
// waveform on the LCD -> press again -> stop, finalize file -> hold the
// button from idle to open a menu (upload now / wifi info / storage /
// about) navigated with short press (next item) and long press (select).
//
// Before this compiles/works on real hardware:
//   1. Fill in the TODOs in pins.h with your board's real GPIO numbers.
//   2. Fill in config.h with your WiFi credentials (server URL already
//      points at the savage.local Docker deployment - see server/deploy.sh).
//   3. If audio is silent/garbled once everything else works, see the note
//      at the top of es8311_codec.h about MCLK/PLL settings.
//
// I2S note: this uses arduino-esp32's ESP_I2S.h wrapper (I2SClass), which is
// the supported path for capturing audio on the ESP32-C6 under Arduino. Its
// exact method signatures have shifted a bit across arduino-esp32 core
// versions - if something here doesn't compile against your installed core,
// check File > Examples > ESP_I2S in the Arduino/PlatformIO package for the
// current signatures; the rest of this firmware (SD/WAV/LCD/upload/state
// machine) doesn't depend on which I2S API revision you're on.

#include <Arduino.h>
#include <ESP_I2S.h>
#include <WiFi.h>
#include <SD.h>
#include <vector>

#include "pins.h"
#include "config.h"
#include "es8311_codec.h"
#include "rtc_pcf85063.h"
#include "wav_recorder.h"
#include "ui_display.h"
#include "record_button.h"
#include "wifi_uploader.h"
#include "audio_visualizer.h"

enum class State { IDLE, RECORDING, UPLOADING, MENU, INFO };

static State state = State::IDLE;

static I2SClass i2s;
static ES8311 codec;
static Pcf85063 rtc;
static WavRecorder recorder;
static UiDisplay ui;
static RecordButton button;
static WifiUploader uploader;
static AudioVisualizer visualizer;

static unsigned long recordingStartMs = 0;
static unsigned long lastIdleRedrawMs = 0;
static unsigned long lastUploadAttemptMs = 0;

static const std::vector<MenuItem> kMenuItems = {
  {"Upload now"},
  {"WiFi info"},
  {"Storage"},
  {"About"},
  {"Exit"},
};
static int menuSelectedIndex = 0;
static unsigned long menuLastActivityMs = 0;

// Reads the battery percentage. The ETA6098 charge IC handles charging in
// hardware; this assumes the board exposes a battery-voltage sense pin on
// an ADC channel. If it doesn't, hardcode a value or remove battery display
// from ui_display.h - check the schematic for a "BAT_ADC" style pin.
int readBatteryPercent() {
  // TODO - CONFIRM there is a battery-sense ADC pin, and its GPIO number.
  // Placeholder: report unknown as 100% rather than a misleading reading.
  return 100;
}

bool hasPendingUploads() {
  return !WavRecorder::pendingFiles().empty();
}

void startRecording() {
  String base = rtc.filenameTimestamp();
  if (!recorder.startNewFile(base)) {
    ui.showMessage("SD write failed!");
    delay(1500);
    return;
  }
  codec.mute(false);
  visualizer.reset();
  recordingStartMs = millis();
  state = State::RECORDING;
}

void stopRecording() {
  recorder.close();
  codec.mute(true);
  state = State::IDLE;
}

void pumpAudioToSd() {
  // Pull available I2S samples, write them to the open WAV file, and feed
  // the same bytes to the visualizer so the waveform reflects what's
  // actually being recorded (not a separate/fake animation).
  static uint8_t buf[2048];
  size_t bytesRead = i2s.readBytes((char *)buf, sizeof(buf));
  if (bytesRead > 0) {
    recorder.write(buf, bytesRead);
    visualizer.feed(buf, bytesRead);
  }
}

void tryUpload() {
  auto pending = WavRecorder::pendingFiles();
  if (pending.empty()) return;

  state = State::UPLOADING;
  ui.showUploading(0, (int)pending.size());

  if (!uploader.connect()) {
    ui.showMessage("WiFi unavailable");
    delay(1500);
    state = State::IDLE;
    return;
  }

  uploader.uploadAllPending(pending, [](int done, int total) {
    ui.showUploading(done, total);
  });

  uploader.disconnect();
  state = State::IDLE;
}

void enterMenu() {
  menuSelectedIndex = 0;
  menuLastActivityMs = millis();
  state = State::MENU;
  ui.showMenu(kMenuItems, menuSelectedIndex);
}

void showWifiInfoScreen() {
  std::vector<String> lines;
  lines.push_back(String("SSID: ") + WIFI_SSID);
  lines.push_back(WiFi.status() == WL_CONNECTED
                       ? ("IP: " + WiFi.localIP().toString())
                       : String("Not connected"));
  lines.push_back("(WiFi only powers on");
  lines.push_back(" to upload recordings)");
  ui.showInfo("WIFI", lines);
}

void showStorageInfoScreen() {
  std::vector<String> lines;
  uint64_t totalMb = SD.totalBytes() / (1024ULL * 1024ULL);
  uint64_t usedMb = SD.usedBytes() / (1024ULL * 1024ULL);
  char buf[32];
  snprintf(buf, sizeof(buf), "%llu / %llu MB used", usedMb, totalMb);
  lines.push_back(buf);

  int pendingCount = (int)WavRecorder::pendingFiles().size();
  snprintf(buf, sizeof(buf), "%d recording(s) pending upload", pendingCount);
  lines.push_back(buf);
  ui.showInfo("STORAGE", lines);
}

void showAboutScreen() {
  std::vector<String> lines;
  lines.push_back(String("AI Meeting Buddy v") + DEVICE_VERSION);
  lines.push_back("Waveshare ESP32-C6-LCD-1.69");
  ui.showInfo("ABOUT", lines);
}

// Executes the currently-selected menu item, then decides what state comes next.
void runSelectedMenuItem() {
  String label = kMenuItems[menuSelectedIndex].label;

  if (label == "Upload now") {
    tryUpload();  // sets state internally (UPLOADING -> IDLE)
    return;
  }
  if (label == "Exit") {
    state = State::IDLE;
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

  rtc.begin();
  codec.begin(SAMPLE_RATE_HZ);
  codec.setMicGain(4);
  codec.mute(true);

  i2s.setPins(PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT, PIN_I2S_DIN, PIN_I2S_MCLK);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE_HZ, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    Serial.println("I2S init failed");
  }

  if (!recorder.beginSd()) {
    Serial.println("SD init failed - check wiring/pins.h");
  }

  if (!ui.begin()) {
    Serial.println("LCD init failed - check wiring/pins.h");
  }

  button.begin();

  ui.showMessage("AI Meeting Buddy\nstarting up...");
  delay(800);
}

void loop() {
  ButtonEvent ev = button.poll();

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
      if (millis() - lastIdleRedrawMs > 1000) {
        lastIdleRedrawMs = millis();
        RtcTime t;
        char clockBuf[16] = "--:--";
        if (rtc.getTime(t)) {
          snprintf(clockBuf, sizeof(clockBuf), "%02d:%02d", t.hour, t.minute);
        }
        ui.showIdle(clockBuf, readBatteryPercent(), WiFi.status() == WL_CONNECTED, hasPendingUploads());
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
      if (millis() - lastRedraw > 120) {  // fast enough for the waveform to feel live
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
        menuSelectedIndex = (menuSelectedIndex + 1) % kMenuItems.size();
        menuLastActivityMs = millis();
        ui.showMenu(kMenuItems, menuSelectedIndex);
      } else if (ev == ButtonEvent::LONG) {
        menuLastActivityMs = millis();
        runSelectedMenuItem();
      } else if (millis() - menuLastActivityMs > MENU_IDLE_TIMEOUT_MS) {
        state = State::IDLE;  // walked away / forgot about it
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
        state = State::IDLE;
      }
      break;
    }
  }
}
