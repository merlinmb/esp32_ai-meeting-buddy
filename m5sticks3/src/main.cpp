// AI Meeting Buddy - firmware for M5 StickS3
//
// Two dedicated buttons, no hold gesture: G11 selects/records, G12
// navigates. Flow: idle screen -> press G11 -> record to SD as WAV, with a
// live waveform on the LCD -> press again -> stop, finalize file -> press
// G12 from idle to open a menu (playback / network / storage / about)
// navigated with G12 (next item) and G11 (select). Network opens a submenu
// (connect to saved WiFi / scan+add a new one / upload now / disconnect /
// clear saved networks); adding a new network types its password on an
// on-screen keyboard grid, navigated the same next/select way.
// Pushbutton rotary encoder on pins 0/1/8 mirrors both (rotation = G12,
// click = G11).
//
// Playback (from the menu): rotate the encoder (or press G12) to browse
// recordings, press G11 to play the highlighted one, press again to
// pause/resume, shake the device (Y axis) to back out.
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
#include "driver/gpio.h"
#include "esp_sleep.h"

#include "pins.h"
#include "config.h"
#include "ntp_clock.h"
#include "wav_recorder.h"
#include "wav_player.h"
#include "ui_display.h"
#include "record_button.h"
#include "wifi_uploader.h"
#include "wifi_store.h"
#include "audio_visualizer.h"
#include "rotary_encoder.h"
#include "sounds.h"

enum class State { IDLE, RECORDING, UPLOADING, MENU, INFO, PLAYBACK_LIST, PLAYING, STORAGE_MENU,
                    NETWORK_MENU, NETWORK_LIST, NETWORK_KEYBOARD };

static State state = State::IDLE;

static NtpClock clock_;
static WavRecorder recorder;
static UiDisplay ui;
static RecordButton button(PIN_RECORD_BUTTON, RECORD_BUTTON_ACTIVE_LOW);
static NavButton navButton(PIN_NAV_BUTTON, NAV_BUTTON_ACTIVE_LOW);
static RotaryEncoder encoder;
static WifiUploader uploader;
static WifiStore wifiStore;
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

// M5Unified's StickS3 board profile doesn't wire up a wakeup pin (unlike
// e.g. CoreS3), so M5.Power.lightSleep()'s own touch_wakeup only arms
// whatever board pin M5Unified knows about - nothing, here. Arm G11/G12
// directly instead so the same two buttons that already wake the screen
// (see the SCREEN_TIMEOUT_MS handling in loop()) also wake the device from
// light sleep. Unlike deep sleep, light sleep returns right here on wake
// rather than restarting the program, so loop() picks back up normally.
void enterLightSleep() {
  gpio_wakeup_enable((gpio_num_t)PIN_RECORD_BUTTON, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)PIN_NAV_BUTTON, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();

  M5.Power.lightSleep(M5.Power.sleep_no_timer, true);

  gpio_wakeup_disable((gpio_num_t)PIN_RECORD_BUTTON);
  gpio_wakeup_disable((gpio_num_t)PIN_NAV_BUTTON);

  lastActivityMs = millis();
  if (!screenOn) {
    screenOn = true;
    M5.Lcd.wakeup();
    lastIdleRedrawMs = 0;
  }
  forceIdleRedraw = true;
}
// Set on entry into RECORDING so showRecording() draws its static chrome
// once instead of every ~120ms redraw - see the RECORDING case in loop().
static bool firstRecordingDraw = true;

static std::vector<MenuItem> kMenuItems = {
  {"Playback", Glyph::kPlay},
  {"Network", Glyph::kWifi},
  {"Storage", Glyph::kDisk},  // flips to a critical glyph in refreshMenuIcons() when SD is missing
  {"About", Glyph::kInfo},
  {"Power off", Glyph::kPower},
  {"Exit", Glyph::kExit},
};
static int menuSelectedIndex = 0;
static unsigned long menuLastActivityMs = 0;

// Storage submenu: "Clear recordings" is destructive, so it always sits
// above a dedicated "Back" item (never combined with the parent menu's own
// back/exit path) so backing out of Storage can't double as an accidental
// tap on "clear everything".
static std::vector<MenuItem> kStorageMenuItems = {
  {"Storage info", Glyph::kDisk},
  {"Clear recordings", Glyph::kCrit},
  {"Back", Glyph::kExit},
};
static int storageMenuSelectedIndex = 0;
// Which menu an INFO/result screen backs out to - the main menu, Storage, or
// Network submenu each show their own result screens (e.g. "Storage
// cleared", "Connected") via the shared INFO state, so backing out needs to
// know which one launched it.
enum class InfoReturnTo { kMainMenu, kStorageMenu, kNetworkMenu };
static InfoReturnTo infoReturnTo = InfoReturnTo::kMainMenu;

// Network submenu: "Clear WiFis" is destructive (wipes every saved
// network), so - same convention as kStorageMenuItems - it sits below the
// other actions with "Back" last, never combined with a normal action.
static std::vector<MenuItem> kNetworkMenuItems = {
  {"WiFi", Glyph::kWifi},
  {"Add new WiFi", Glyph::kWifi},
  {"Upload now", Glyph::kUpload},
  {"Disconnect", Glyph::kWifi},
  {"Clear WiFis", Glyph::kCrit},
  {"Back", Glyph::kExit},
};
static int networkMenuSelectedIndex = 0;

static std::vector<String> networkScanList;
static std::vector<bool> networkScanSaved;
static int networkListSelectedIndex = 0;
static unsigned long networkListActivityMs = 0;

// Keyboard cursor: (col,row), with row == UiDisplay's kActionRow meaning the
// SPACE/OK/CANCEL row (col clamped to 0..2 there instead of 0..kKeyCols-1).
static int kbCol = 0, kbRow = 0;
static bool kbShift = false;
static String kbText;
static String kbPendingSsid;  // network the keyboard's password is for

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

// True while USB (VBUS) is present, via the M5PM1 PMIC's VBUS monitor
// (getVBUSVoltage() returns -1 on boards without VBUS sensing).
bool isUsbConnected() {
  return M5.Power.getVBUSVoltage() > 0;
}

// Prefers the last-saved WiFi network (set via the Network menu); falls back
// to config.h's WIFI_SSID/WIFI_PASSWORD if nothing's been saved yet, so a
// fresh device still connects out of the box without a trip through the menu.
bool connectPreferredOrDefault() {
  String ssid = wifiStore.preferredSsid();
  if (ssid.length()) {
    String pass;
    wifiStore.findPassword(ssid, pass);
    if (uploader.connect(ssid, pass)) return true;
  }
  return uploader.connect();
}

bool hasPendingUploads() {
  return sdOk && !WavRecorder::pendingFiles().empty();
}

void startRecording() {
  // SD init at boot can fail if the card wasn't seated yet; retry here so
  // inserting a card and pressing record recovers without a reboot.
  if (!sdOk) {
    sdOk = recorder.beginSd();
    refreshMenuIcons();
  }
  if (!sdOk) {
    Sounds::error();
    ui.showStatus(Severity::kCrit, "No SD card",
                  {"Recordings can't be saved.", "", "Insert a card, then press to retry."},
                  "retry", false);
    delay(1500);
    forceIdleRedraw = true;
    return;
  }
  String base = clock_.filenameTimestamp();
  if (!recorder.startNewFile(base)) {
    Sounds::error();
    ui.showStatus(Severity::kCrit, "Save failed",
                  {"Recording stopped. Last few seconds may be lost.", "", "Check SD space, then retry."},
                  "retry", false);
    delay(1500);
    forceIdleRedraw = true;
    return;
  }
  visualizer.reset();
  recordingStartMs = millis();
  state = State::RECORDING;
  firstRecordingDraw = true;
  Sounds::recordStart();
}

void stopRecording() {
  uint32_t elapsed = (millis() - recordingStartMs) / 1000;
  recorder.close();
  if (elapsed < MIN_RECORDING_SECONDS) {
    recorder.discardLast();  // too short to be a real recording - drop it
  } else {
    lastRecordingSeconds = elapsed;
  }
  M5.Mic.end();  // release I2S/codec between recordings to save power
  // Mic and Speaker share one ES8311 codec chip; M5.Mic.end() powers the
  // whole codec down via its own enable callback, but M5Unified's
  // Speaker_Class::begin() only re-runs *its* codec-enable sequence the
  // very first time it's called (it no-ops if already "begun"). Without
  // this, the speaker's _begun flag stays true forever after the first
  // tone played at boot, so nothing re-powers the codec's DAC/output path
  // after a recording - every tone() and WAV playback after that goes
  // silent. Ending the speaker here forces it to properly re-begin (and
  // re-enable the codec) on its next use.
  M5.Speaker.end();
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
  // The panel controller ignores/garbles writes while asleep (see the wake
  // handling in loop()), so don't draw at all while screenOn is false - the
  // upload still runs to completion in the background either way, and if
  // nothing was drawn there's no result screen to leave up afterward either
  // (see the screenOn check at the end of this function).
  bool uiDrawnThisUpload = false;
  if (screenOn) {
    ui.showUploading(0, (int)pending.size(), true);
    uiDrawnThisUpload = true;
  }

  if (!connectPreferredOrDefault()) {
    Sounds::error();
    if (screenOn) {
      ui.showStatus(Severity::kWarn, "Wi-Fi offline",
                    {"Recording still saves to SD.", "", "Uploads once Wi-Fi reconnects."},
                    "ok", false);
      delay(1500);
    }
    goIdle();
    return;
  }

  int chunkProgressDoneCount = 0;
  int uploadedCount = uploader.uploadAllPending(
      pending,
      [&chunkProgressDoneCount, &uiDrawnThisUpload, &pending](int done, int total) {
        chunkProgressDoneCount = done;
        if (!screenOn) return;
        ui.showUploading(done, total, !uiDrawnThisUpload);
        uiDrawnThisUpload = true;
      },
      [&pending, &chunkProgressDoneCount, &uiDrawnThisUpload](size_t sentBytes, size_t fileSize) {
        if (!screenOn) return;
        // Uses the last-completed-file count so the bar's file-count text
        // stays correct while its fill advances mid-file.
        float fraction = fileSize > 0 ? (float)sentBytes / (float)fileSize : 0.0f;
        ui.showUploading(chunkProgressDoneCount, (int)pending.size(), !uiDrawnThisUpload, fraction);
        uiDrawnThisUpload = true;
      });

  // Leave WiFi associated while USB is connected so the next periodic
  // upload retry (or "Upload now") skips the reconnect/handshake cost -
  // there's no battery cost to justify tearing it down while powered.
  if (!isUsbConnected()) uploader.disconnect();

  // uploadAllPending() leaves any file that failed (server unreachable,
  // rejected it, etc) still marked pending for the next retry - so a run
  // where nothing actually succeeded isn't a completion, it's the same
  // stuck files being retried again. Only chime/report success when at
  // least one file really moved to /uploaded; otherwise treat it like the
  // Wi-Fi-offline case below instead of announcing an upload that didn't
  // happen.
  if (uploadedCount == 0) {
    Sounds::error();
    if (screenOn) {
      ui.showStatus(Severity::kWarn, "Upload failed",
                    {"Recording still saves to SD.", "", "Will retry automatically."},
                    "ok", false);
      delay(1500);
    }
    goIdle();
    return;
  }

  Sounds::uploadComplete();

  // If the screen is off, there's no one to show the result to - go
  // straight back to idle instead of leaving a result screen up that would
  // otherwise show stale/blank GRAM the next time the display wakes (see
  // the wake handling in loop(), which only knows how to repaint IDLE and
  // RECORDING). Idle's own Wi-Fi/SD status rows already reflect the outcome.
  if (!screenOn) {
    goIdle();
    return;
  }

  // Stay on a result screen instead of dropping straight back to idle -
  // the state machine's own fillScreen()-on-entry (goIdle()'s
  // forceIdleRedraw) already keeps the eventual idle redraw clean; this is
  // about giving the user a moment to see the upload actually finished
  // before it disappears, and something to acknowledge (G11 back) rather
  // than auto-dismissing.
  char buf[24];
  snprintf(buf, sizeof(buf), "%d file%s uploaded", uploadedCount, uploadedCount == 1 ? "" : "s");
  ui.showStatus(Severity::kGood, "Upload complete", {buf}, "back", false);
  state = State::UPLOADING;
  menuLastActivityMs = millis();
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
                  "back", false);
    delay(1000);
    ui.showRecordingList(playbackList, playbackSelectedIndex);
    return;
  }
  playbackLastActivityMs = millis();
  state = State::PLAYING;
  ui.showPlayback(player.path(), 0, player.totalSeconds(), false, true);
}

void enterMenu() {
  menuSelectedIndex = 0;
  menuLastActivityMs = millis();
  state = State::MENU;
  refreshMenuIcons();
  ui.showMenu(kMenuItems, menuSelectedIndex);
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

void enterStorageMenu() {
  // SD init at boot can fail if the card wasn't seated yet; retry here so
  // opening Storage after inserting a card recovers without a reboot.
  if (!sdOk) {
    sdOk = recorder.beginSd();
    refreshMenuIcons();
  }
  storageMenuSelectedIndex = 0;
  menuLastActivityMs = millis();
  state = State::STORAGE_MENU;
  ui.showMenu(kStorageMenuItems, storageMenuSelectedIndex);
}

// Deletes every recording on the card. Reachable only via the Storage
// submenu's dedicated "Clear recordings" item, with "Back" as a separate,
// harmless item below it - see kStorageMenuItems.
void clearAllRecordings() {
  int count = WavRecorder::deleteAllRecordings();
  Sounds::recordEnd();
  char buf[32];
  snprintf(buf, sizeof(buf), "%d recording(s) deleted", count);
  ui.showStatus(Severity::kGood, "Storage cleared", {buf}, "back", false);
  state = State::INFO;
  infoReturnTo = InfoReturnTo::kStorageMenu;
  menuLastActivityMs = millis();
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

void enterNetworkMenu();  // defined below; used here and by the INFO state's back-navigation

// Executes the currently-selected menu item, then decides what state comes next.
void runSelectedMenuItem() {
  String label = kMenuItems[menuSelectedIndex].label;

  if (label == "Playback") {
    enterPlaybackList();
    return;
  }
  if (label == "Network") {
    enterNetworkMenu();
    return;
  }
  if (label == "Exit") {
    goIdle();
    return;
  }
  if (label == "Storage") {
    enterStorageMenu();
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
  infoReturnTo = InfoReturnTo::kMainMenu;
  if (label == "About") showAboutScreen();
}

// Executes the currently-selected item in the Storage submenu.
void runSelectedStorageMenuItem() {
  String label = kStorageMenuItems[storageMenuSelectedIndex].label;
  if (label == "Storage info") {
    state = State::INFO;
    infoReturnTo = InfoReturnTo::kStorageMenu;
    showStorageInfoScreen();
    return;
  }
  if (label == "Clear recordings") {
    clearAllRecordings();
    return;
  }
  // "Back"
  enterMenu();
}

// Connects using the given ssid/password, showing a good/warn showStatus()
// result screen either way - same pattern as tryUpload()'s WiFi-offline
// screen. "back" on the result screen returns to the Network submenu.
void connectAndReport(const String &ssid, const String &password) {
  ui.showMessage("Connecting...", {ssid});
  bool ok = uploader.connect(ssid, password);
  if (ok) {
    Sounds::recordEnd();
    ui.showStatus(Severity::kGood, "Connected", {ssid}, "back", false);
  } else {
    Sounds::error();
    ui.showStatus(Severity::kWarn, "Couldn't connect", {"Check the password", "and try again."}, "back", false);
  }
  state = State::INFO;
  infoReturnTo = InfoReturnTo::kNetworkMenu;
  menuLastActivityMs = millis();
}

void enterNetworkMenu() {
  networkMenuSelectedIndex = 0;
  menuLastActivityMs = millis();
  state = State::NETWORK_MENU;
  ui.showMenu(kNetworkMenuItems, networkMenuSelectedIndex);
}

void enterNetworkList() {
  ui.showMessage("Scanning...", {});
  uploader.scanNetworks(networkScanList);
  networkScanSaved.clear();
  for (auto &ssid : networkScanList) {
    String pass;
    networkScanSaved.push_back(wifiStore.findPassword(ssid, pass));
  }
  networkListSelectedIndex = 0;
  networkListActivityMs = millis();
  state = State::NETWORK_LIST;
  ui.showNetworkList(networkScanList, networkListSelectedIndex, networkScanSaved);
}

void enterPasswordKeyboard(const String &ssid) {
  kbPendingSsid = ssid;
  kbText = "";
  kbShift = false;
  kbCol = 0;
  kbRow = 0;
  state = State::NETWORK_KEYBOARD;
  ui.showKeyboard("PASSWORD", kbText, true, kbShift, kbCol, kbRow);
}

// G11/select on the currently-highlighted scanned network: connect
// immediately with a saved password, otherwise prompt for one.
void runSelectedNetworkListItem() {
  if (networkScanList.empty()) return;
  String ssid = networkScanList[networkListSelectedIndex];
  String pass;
  if (wifiStore.findPassword(ssid, pass)) {
    connectAndReport(ssid, pass);
    return;
  }
  enterPasswordKeyboard(ssid);
}

// Called when OK is selected on the password keyboard: connects with the
// typed password and - only on success - saves it as the new preferred
// network (a wrong password shouldn't get remembered).
void submitKeyboardPassword() {
  bool ok = uploader.connect(kbPendingSsid, kbText);
  if (ok) {
    wifiStore.save(kbPendingSsid, kbText);
    Sounds::recordEnd();
    ui.showStatus(Severity::kGood, "Connected", {kbPendingSsid}, "back", false);
  } else {
    Sounds::error();
    ui.showStatus(Severity::kWarn, "Couldn't connect", {"Check the password", "and try again."}, "back", false);
  }
  state = State::INFO;
  infoReturnTo = InfoReturnTo::kNetworkMenu;
  menuLastActivityMs = millis();
}

// Executes the currently-selected item in the Network submenu.
void runSelectedNetworkMenuItem() {
  String label = kNetworkMenuItems[networkMenuSelectedIndex].label;
  if (label == "WiFi") {
    String ssid = wifiStore.preferredSsid();
    if (!ssid.length()) {
      ui.showStatus(Severity::kWarn, "No saved network", {"Use \"Add new WiFi\"", "to connect first."}, "back", false);
      state = State::INFO;
      infoReturnTo = InfoReturnTo::kNetworkMenu;
      menuLastActivityMs = millis();
      return;
    }
    String pass;
    wifiStore.findPassword(ssid, pass);
    connectAndReport(ssid, pass);
    return;
  }
  if (label == "Add new WiFi") {
    enterNetworkList();
    return;
  }
  if (label == "Upload now") {
    tryUpload();  // sets state internally (UPLOADING -> IDLE)
    return;
  }
  if (label == "Disconnect") {
    uploader.disconnect();
    ui.showStatus(Severity::kInfo, "Disconnected", {}, "back", false);
    state = State::INFO;
    infoReturnTo = InfoReturnTo::kNetworkMenu;
    menuLastActivityMs = millis();
    return;
  }
  if (label == "Clear WiFis") {
    wifiStore.clear();
    ui.showStatus(Severity::kGood, "WiFi networks cleared", {}, "back", false);
    state = State::INFO;
    infoReturnTo = InfoReturnTo::kNetworkMenu;
    menuLastActivityMs = millis();
    return;
  }
  // "Back"
  enterMenu();
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
  navButton.begin();
  encoder.begin();
  ui.bootLog("Controls ready", Severity::kGood);

  // Best-effort: no RTC chip on this board, so get wall-clock time from
  // NTP if WiFi is reachable. Recording still works if this fails/times
  // out - filenameTimestamp() falls back to a millis()-based name.
  if (connectPreferredOrDefault()) {
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
  // Two dedicated buttons, no hold gesture: G11 selects/records, G12
  // navigates. The rotary encoder mirrors both - rotation is the same as a
  // G12 press (with direction for menuStepBack), its own click is the same
  // as a G11 press.
  bool select = (button.poll() == ButtonEvent::PRESSED);
  bool nav = (navButton.poll() == ButtonEvent::PRESSED);
  bool menuStepBack = false;
  switch (encoder.poll()) {
    case EncoderEvent::RIGHT:
      nav = true;
      break;
    case EncoderEvent::LEFT:
      nav = true;
      menuStepBack = true;
      break;
    case EncoderEvent::BUTTON:
      select = true;
      break;
    case EncoderEvent::NONE:
      break;
  }

  // Shake-to-go-back: a hard flick on the Y axis backs out one level, same
  // as it always has, from any screen except IDLE/RECORDING (where a shake
  // is more likely to be incidental handling, not a deliberate gesture).
  bool shakeBack = detectShake();

  // Backlight power save: any real input wakes the screen and resets the
  // inactivity timer. In IDLE the input that woke the screen is consumed
  // here and does NOT also perform its normal action, so a pocket bump
  // can't start a recording or open the menu - it takes a second,
  // deliberate input. RECORDING is the exception: the button press is left
  // to fall through and stop the recording immediately, since requiring a
  // second press to stop would mean the mic silently keeps recording after
  // the user believes they've stopped it. A waking shake in RECORDING is
  // still swallowed, same as before, since it's more likely incidental
  // handling than a deliberate gesture.
  bool hadInput = select || nav || shakeBack;
  bool woke = false;
  if (hadInput) {
    lastActivityMs = millis();
    if (!screenOn) {
      screenOn = true;
      M5.Lcd.wakeup();
      woke = true;
      lastIdleRedrawMs = 0;  // force an immediate redraw instead of waiting out the 1s throttle
      // sleep()/wakeup() leaves stale GRAM content, so whichever blankable
      // screen we wake back into must fully repaint its chrome, not just its
      // normal partial/incremental update.
      forceIdleRedraw = true;
      firstRecordingDraw = true;
    }
  }
  bool blankableState = (state == State::IDLE || state == State::RECORDING);
  if (blankableState && screenOn && !isUsbConnected() && (millis() - lastActivityMs > SCREEN_TIMEOUT_MS)) {
    screenOn = false;
    M5.Lcd.sleep();
  }
  if (woke && state == State::IDLE) {
    select = false;
    nav = false;
    shakeBack = false;
  }
  if (woke && state == State::RECORDING) {
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
      // G12 (or encoder rotation) opens the menu - "next item" only makes
      // sense once there's a list/menu on screen, and it's the dedicated
      // navigation input. G11 (or the encoder's own click) starts a
      // recording, its only job on this screen.
      if (nav) {
        enterMenu();
        break;
      }
      if (select) {
        startRecording();
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
      if (!isUsbConnected() && (millis() - lastActivityMs > SLEEP_TIMEOUT_MS)) {
        enterLightSleep();
      }
      break;
    }

    case State::RECORDING: {
      pumpAudioToSd();
      if (select || nav) {
        stopRecording();
        break;
      }
      static unsigned long lastRedraw = 0;
      if (screenOn && millis() - lastRedraw > 120) {  // fast enough for the waveform to feel live
        lastRedraw = millis();
        ui.showRecording((millis() - recordingStartMs) / 1000, visualizer, firstRecordingDraw);
        firstRecordingDraw = false;
      }
      break;
    }

    case State::UPLOADING: {
      // tryUpload() runs synchronously and leaves the "Upload complete"
      // result screen showing when it returns; from here it's just a
      // dismissible status screen like INFO, waiting for G11 (or a timeout
      // if the user walks away) before returning to idle.
      if (select) {
        goIdle();
      } else if (millis() - menuLastActivityMs > MENU_IDLE_TIMEOUT_MS) {
        goIdle();
      }
      break;
    }

    case State::MENU: {
      if (nav) {
        // G12 (or encoder rotation): move the highlight only.
        int count = (int)kMenuItems.size();
        menuSelectedIndex = menuStepBack
                                ? (menuSelectedIndex + count - 1) % count
                                : (menuSelectedIndex + 1) % count;
        menuLastActivityMs = millis();
        ui.showMenu(kMenuItems, menuSelectedIndex);
      } else if (select) {
        // G11 (or encoder click): select the highlighted item.
        menuLastActivityMs = millis();
        runSelectedMenuItem();
      } else if (millis() - menuLastActivityMs > MENU_IDLE_TIMEOUT_MS) {
        goIdle();  // walked away / forgot about it
      }
      break;
    }

    case State::INFO: {
      // G11 from an info screen goes back to whichever menu it was opened
      // from (main menu, Storage submenu, or Network submenu); anything else
      // (or a timeout) also bails out to keep the UI from ever feeling stuck
      // on one screen.
      if (select || nav) {
        if (infoReturnTo == InfoReturnTo::kStorageMenu) enterStorageMenu();
        else if (infoReturnTo == InfoReturnTo::kNetworkMenu) enterNetworkMenu();
        else enterMenu();
      } else if (millis() - menuLastActivityMs > MENU_IDLE_TIMEOUT_MS) {
        goIdle();
      }
      break;
    }

    case State::STORAGE_MENU: {
      if (nav) {
        // G12 (or encoder rotation): move the highlight only.
        int count = (int)kStorageMenuItems.size();
        storageMenuSelectedIndex = menuStepBack
                                        ? (storageMenuSelectedIndex + count - 1) % count
                                        : (storageMenuSelectedIndex + 1) % count;
        menuLastActivityMs = millis();
        ui.showMenu(kStorageMenuItems, storageMenuSelectedIndex);
      } else if (select) {
        // G11 (or encoder click): select the highlighted item.
        menuLastActivityMs = millis();
        runSelectedStorageMenuItem();
      } else if (shakeBack) {
        enterMenu();
      } else if (millis() - menuLastActivityMs > MENU_IDLE_TIMEOUT_MS) {
        goIdle();
      }
      break;
    }

    case State::NETWORK_MENU: {
      if (nav) {
        // G12 (or encoder rotation): move the highlight only.
        int count = (int)kNetworkMenuItems.size();
        networkMenuSelectedIndex = menuStepBack
                                        ? (networkMenuSelectedIndex + count - 1) % count
                                        : (networkMenuSelectedIndex + 1) % count;
        menuLastActivityMs = millis();
        ui.showMenu(kNetworkMenuItems, networkMenuSelectedIndex);
      } else if (select) {
        // G11 (or encoder click): select the highlighted item.
        menuLastActivityMs = millis();
        runSelectedNetworkMenuItem();
      } else if (shakeBack) {
        enterMenu();
      } else if (millis() - menuLastActivityMs > MENU_IDLE_TIMEOUT_MS) {
        goIdle();
      }
      break;
    }

    case State::NETWORK_LIST: {
      if (nav) {
        // G12 (or encoder rotation): move the highlight only.
        if (!networkScanList.empty()) {
          int count = (int)networkScanList.size();
          networkListSelectedIndex = menuStepBack
                                          ? (networkListSelectedIndex + count - 1) % count
                                          : (networkListSelectedIndex + 1) % count;
        }
        networkListActivityMs = millis();
        ui.showNetworkList(networkScanList, networkListSelectedIndex, networkScanSaved);
      } else if (select) {
        // G11 (or encoder click): connect to (or prompt a password for) the highlighted network.
        networkListActivityMs = millis();
        runSelectedNetworkListItem();
      } else if (shakeBack) {
        enterNetworkMenu();
      } else if (millis() - networkListActivityMs > MENU_IDLE_TIMEOUT_MS) {
        goIdle();
      }
      break;
    }

    case State::NETWORK_KEYBOARD: {
      // nav/menuStepBack move the cursor cell-by-cell through the grid,
      // wrapping row-to-row (matching Bruce's generalKeyboard() NEXT/PREV
      // behavior) rather than a 1D index, since the keyboard is a genuine
      // 2D layout. The action row (SPACE/OK/CANCEL) has only 3 columns, so
      // entering/leaving it clamps col instead of reusing kKeyCols.
      if (nav) {
        int maxCol = (kbRow == UiDisplay::kActionRow) ? 2 : (UiDisplay::kKeyCols - 1);
        if (menuStepBack) {
          kbCol--;
          if (kbCol < 0) {
            kbRow = (kbRow <= 0) ? UiDisplay::kActionRow : kbRow - 1;
            kbCol = (kbRow == UiDisplay::kActionRow) ? 2 : (UiDisplay::kKeyCols - 1);
          }
        } else {
          kbCol++;
          if (kbCol > maxCol) {
            kbCol = 0;
            kbRow = (kbRow >= UiDisplay::kActionRow) ? 0 : kbRow + 1;
          }
        }
        ui.showKeyboard("PASSWORD", kbText, true, kbShift, kbCol, kbRow);
      } else if (select) {
        if (kbRow == UiDisplay::kActionRow) {
          if (kbCol == 0) {  // SPACE
            kbText += ' ';
            ui.showKeyboard("PASSWORD", kbText, true, kbShift, kbCol, kbRow);
          } else if (kbCol == 1) {  // OK
            submitKeyboardPassword();
          } else {  // CANCEL
            enterNetworkList();
          }
        } else {
          char k = ui.keyAt(kbRow, kbCol, kbShift);
          if (k == UiDisplay::kKeyShift) {
            kbShift = !kbShift;
          } else if (k == UiDisplay::kKeyBackspace) {
            if (kbText.length()) kbText.remove(kbText.length() - 1);
          } else if (k) {
            kbText += k;
          }
          ui.showKeyboard("PASSWORD", kbText, true, kbShift, kbCol, kbRow);
        }
      } else if (shakeBack) {
        enterNetworkList();
      }
      break;
    }

    case State::PLAYBACK_LIST: {
      // The list has one extra row past the recordings themselves - a
      // trailing "Back" entry (see ui.showRecordingList()) - so there's
      // always a deliberate way out of this screen via G11, not just the
      // shake gesture or the idle timeout.
      int rowCount = (int)playbackList.size() + 1;
      if (nav) {
        // G12 (or encoder rotation): move the highlight only.
        playbackSelectedIndex = menuStepBack
                                     ? (playbackSelectedIndex + rowCount - 1) % rowCount
                                     : (playbackSelectedIndex + 1) % rowCount;
        playbackLastActivityMs = millis();
        ui.showRecordingList(playbackList, playbackSelectedIndex);
      } else if (select) {
        // G11 (or encoder click): play the highlighted item, or back out if
        // the trailing "Back" row is highlighted.
        playbackLastActivityMs = millis();
        if (playbackSelectedIndex >= (int)playbackList.size()) {
          enterMenu();
        } else {
          startPlayback();
        }
      } else if (shakeBack) {
        enterMenu();
      } else if (millis() - playbackLastActivityMs > MENU_IDLE_TIMEOUT_MS) {
        goIdle();
      }
      break;
    }

    case State::PLAYING: {
      if (select) {
        // click = pause, click again = resume
        if (player.isPaused()) player.resume();
        else player.pause();
        playbackLastActivityMs = millis();
        ui.showPlayback(player.path(), player.elapsedSeconds(), player.totalSeconds(), player.isPaused(), true);
        break;
      }
      if (nav || shakeBack) {
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
        ui.showPlayback(player.path(), player.elapsedSeconds(), player.totalSeconds(), player.isPaused(), false);
      }
      break;
    }
  }
}
