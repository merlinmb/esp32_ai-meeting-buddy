#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <atomic>
#include <vector>
#include "config.h"
#include "wav_recorder.h"
#include "wifi_uploader.h"

// Runs the upload queue on its own FreeRTOS task instead of inline in
// loop(), so streaming a 2+ hour recording to WiFi never delays button
// polling or LCD redraws on the main task. The ESP32-C6 only has one
// general-purpose core (plus a tiny low-power core that can't drive WiFi or
// SD/SPI at all), but FreeRTOS still timeslices tasks on that one core, and
// network I/O is mostly waiting on the socket - so this task naturally
// yields back to the main task rather than hogging the CPU.
//
// Recording is this device's primary job: setRecordingActive(true) - call
// it the instant a recording starts, before touching the SD card - makes
// this task abandon whatever it's doing (bounded by one network chunk in
// the normal case, or UPLOAD_SOCKET_TIMEOUT_MS in the worst case if it's
// stuck inside a blocking connect) and park itself until recording stops.
// It doesn't try to finish the current pass first: whatever file was in
// flight is exactly where the resumable protocol (see wifi_uploader.h)
// already tracks it, so nothing is lost - the next idle window just
// resumes from there. The SD card is also fine to pull and read on a PC
// while all this is going on: a missing card just makes every SD call fail
// the same way it already did before this task existed, so a pass fails
// gently and retries later.
//
// Uploads are attempted two ways, both non-blocking from main.cpp's point
// of view: automatically about every UPLOAD_RETRY_INTERVAL_MS whenever
// nothing is recording, and immediately on request (the menu's "Upload
// now" item calls requestUploadNow() - this task owns its own timing, not
// main.cpp). Status is published via atomics so loop() can poll it every
// frame with no lock and no risk of ever blocking on this task.
class UploadWorker {
 public:
  enum class State { PARKED, CONNECTING, UPLOADING, FAILED };

  struct Status {
    State state;
    int doneCount;
    int totalCount;
    int failedCount;
  };

  void begin() {
    _wake = xSemaphoreCreateBinary();
    xTaskCreate(&UploadWorker::taskTrampoline, "uploadWorker", UPLOAD_WORKER_STACK_BYTES, this, 1, nullptr);
  }

  // Call the instant recording starts/stops. Checked constantly by the
  // worker - this means "stand down now", not "finish this chunk first".
  void setRecordingActive(bool active) { _recordingActive.store(active); }

  // Wakes the worker immediately instead of waiting for the next periodic
  // retry. Safe to call from any task.
  void requestUploadNow() {
    if (_wake) xSemaphoreGive(_wake);
  }

  Status getStatus() {
    return Status{_state.load(), _doneCount.load(), _totalCount.load(), _failedCount.load()};
  }

 private:
  SemaphoreHandle_t _wake = nullptr;
  std::atomic<bool> _recordingActive{false};
  std::atomic<State> _state{State::PARKED};
  std::atomic<int> _doneCount{0};
  std::atomic<int> _totalCount{0};
  std::atomic<int> _failedCount{0};
  WifiUploader _uploader;

  static void taskTrampoline(void *arg) {
    static_cast<UploadWorker *>(arg)->taskLoop();
  }

  void taskLoop() {
    for (;;) {
      TickType_t wait = AUTO_UPLOAD_WHEN_IDLE ? pdMS_TO_TICKS(UPLOAD_RETRY_INTERVAL_MS) : portMAX_DELAY;
      xSemaphoreTake(_wake, wait);
      if (_recordingActive.load()) continue;  // don't even start; try again next wake
      runPass();
    }
  }

  void runPass() {
    std::vector<String> pending = WavRecorder::pendingFiles();
    if (pending.empty()) return;
    if (_recordingActive.load()) return;  // recording started while we were scanning - stay parked

    _totalCount.store((int)pending.size());
    _doneCount.store(0);
    _failedCount.store(0);
    _state.store(State::CONNECTING);

    auto shouldAbort = [this]() { return _recordingActive.load(); };

    if (!_uploader.connect(shouldAbort)) {
      _uploader.disconnect();
      bool abortedForRecording = _recordingActive.load();
      _state.store(abortedForRecording ? State::PARKED : State::FAILED);
      if (!abortedForRecording) _failedCount.store((int)pending.size());
      return;
    }

    _state.store(State::UPLOADING);
    int done = 0, failed = 0;
    bool interrupted = false;
    for (auto &path : pending) {
      if (_recordingActive.load()) { interrupted = true; break; }

      WifiUploader::UploadResult result = _uploader.uploadFile(path, shouldAbort);
      if (result == WifiUploader::UploadResult::ABORTED) { interrupted = true; break; }
      if (result == WifiUploader::UploadResult::SUCCESS) {
        WavRecorder::markUploaded(path);
      } else {
        failed++;
      }
      done++;
      _doneCount.store(done);
      _failedCount.store(failed);
    }

    _uploader.disconnect();
    // A pass recording interrupted is never shown as a failure, even if an
    // earlier file in the same pass genuinely failed - it just stays
    // pending and gets a clean, honestly-counted retry once this device is
    // done recording.
    _state.store((interrupted || failed == 0) ? State::PARKED : State::FAILED);
  }
};
