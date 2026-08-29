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
#include <Wire.h>
#include <SD.h>
#include <vector>

#include "pins.h"
#include "config.h"
#include "es8311_codec.h"
#include "rtc_pcf85063.h"
#include "wav_recorder.h"
#include "ui_display.h"
#include "record_button.h"
#include "spi_bus_mutex.h"
#include "upload_worker.h"
#include "audio_visualizer.h"

// The LCD and SD card share a SPI bus (see pins.h and spi_bus_mutex.h); this
// is the one definition of the mutex declared extern there.
SemaphoreHandle_t g_spiBusMutex = nullptr;
void spiBusMutexBegin() { g_spiBusMutex = xSemaphoreCreateMutex(); }

enum class State { IDLE, RECORDING, MENU, INFO };

static State state = State::IDLE;

static I2SClass i2s;
static ES8311 codec;
static Pcf85063 rtc;
static WavRecorder recorder;
static UiDisplay ui;
static RecordButton button;
static UploadWorker uploadWorker;
static AudioVisualizer visualizer;

static bool sdOk = false;
static unsigned long recordingStartMs = 0;
static unsigned long lastIdleRedrawMs = 0;
static unsigned long lastUploadRedrawMs = 0;
static uint32_t lastRecordingSeconds = 0;

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
  return sdOk && !WavRecorder::pendingFiles().empty();
}

void startRecording() {
  // Recording is this device's primary job: tell the upload worker to stand
  // down before touching the SD card at all, so it can't still be mid-chunk
  // (or about to open a file) when the recorder needs the bus a moment
  // later. See upload_worker.h for how quickly it actually reacts to this.
  uploadWorker.setRecordingActive(true);

  if (!sdOk) {
    ui.showMessage("SD card fault!\nInsert a card and\nrestart to record.");
    delay(1500);
    uploadWorker.setRecordingActive(false);
    return;
  }
  String base = rtc.filenameTimestamp();
  if (!recorder.startNewFile(base)) {
    ui.showMessage("SD write failed!");
    delay(1500);
    uploadWorker.setRecordingActive(false);
    return;
  }
  codec.mute(false);
  visualizer.reset();
  recordingStartMs = millis();
  state = State::RECORDING;
}

void stopRecording() {
  lastRecordingSeconds = (millis() - recordingStartMs) / 1000;
  recorder.close();
  codec.mute(true);
  state = State::IDLE;
  uploadWorker.setRecordingActive(false);
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
  if (!sdOk) {
    lines.push_back("SD card fault -");
    lines.push_back("no card detected");
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
    // Non-blocking: just wakes the background upload worker. The idle
    // screen picks up its progress/failure status on its own next redraw.
    uploadWorker.requestUploadNow();
    state = State::IDLE;
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

  // Must exist before anything below touches the LCD or SD card, and
  // definitely before the upload worker task (started at the end of this
  // function) can start doing SD reads concurrently with loop().
  spiBusMutexBegin();

  Wire.begin(PIN_CODEC_I2C_SDA, PIN_CODEC_I2C_SCL);  // shared bus - codec + RTC
  rtc.begin();
  codec.begin(SAMPLE_RATE_HZ);
  codec.setMicGain(4);
  codec.mute(true);

  i2s.setPins(PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT, PIN_I2S_DIN, PIN_I2S_MCLK);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE_HZ, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    Serial.println("I2S init failed");
  }

  sdOk = recorder.beginSd();
  if (!sdOk) {
    Serial.println("SD init failed - check wiring/pins.h");
  }

  if (!ui.begin()) {
    Serial.println("LCD init failed - check wiring/pins.h");
  }

  button.begin();
  uploadWorker.begin();

  ui.showMessage("AI Meeting Buddy\nstarting up...");
  delay(800);
}

void loop() {
  ButtonEvent ev = button.poll();

  switch (state) {
    case State::IDLE: {
      // Recording always wins, instantly - regardless of what the idle
      // screen happens to be showing (clock, upload progress, or a failure
      // message). startRecording() tells the upload worker to stand down;
      // it doesn't need to have finished standing down before we proceed
      // here, since it never touches the LCD and any SD access it's still
      // mid-chunk on serializes safely through the bus mutex.
      if (ev == ButtonEvent::SHORT) {
        startRecording();
        break;
      }
      if (ev == ButtonEvent::LONG) {
        enterMenu();
        break;
      }

      UploadWorker::Status ust = uploadWorker.getStatus();
      if (ust.state != UploadWorker::State::PARKED) {
        // Faster cadence than the plain clock so the progress bar feels
        // live, but still throttled - this poll is cheap (atomics only),
        // but the LCD redraw it triggers isn't free.
        if (millis() - lastUploadRedrawMs > 250) {
          lastUploadRedrawMs = millis();
          if (ust.state == UploadWorker::State::FAILED) {
            ui.showUploadFailed(ust.failedCount, ust.totalCount);
          } else {
            ui.showUploading(ust.doneCount, ust.totalCount);
          }
        }
      } else if (millis() - lastIdleRedrawMs > 1000) {
        lastIdleRedrawMs = millis();
        RtcTime t;
        char clockBuf[16] = "--:--";
        if (rtc.getTime(t)) {
          snprintf(clockBuf, sizeof(clockBuf), "%02d:%02d", t.hour, t.minute);
        }
        float sdFreePct = 0.0f;
        if (sdOk) {
          uint64_t sdTotal = SD.totalBytes();
          uint64_t sdUsed = SD.usedBytes();
          sdFreePct = sdTotal > 0 ? 100.0f * (float)(sdTotal - sdUsed) / (float)sdTotal : 0.0f;
        }
        ui.showIdle(clockBuf, WiFi.status() == WL_CONNECTED, sdOk, sdFreePct, lastRecordingSeconds);
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
