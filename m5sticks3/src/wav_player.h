#pragma once
#include <Arduino.h>
#include <M5Unified.h>
#include <SD.h>
#include <vector>
#include <algorithm>
#include "pins.h"
#include "config.h"

// Plays a WAV file from the SD card through M5.Speaker in small chunks, one
// chunk per pump() call, so it never blocks loop() (button/encoder polling
// keeps working while a recording plays). Mirrors the read-then-decode
// approach from the M5Unified Speaker_SD_wav_file example, but pump()-driven
// like wav_recorder's write side instead of the example's blocking loop.

// Matches the header wav_recorder.h writes (44 bytes, canonical PCM WAV).
#pragma pack(push, 1)
struct WavFileHeader {
  char     riff[4];
  uint32_t chunkSize;
  char     wave[4];
  char     fmt[4];
  uint32_t fmtSize;
  uint16_t audioFormat;
  uint16_t numChannels;
  uint32_t sampleRate;
  uint32_t byteRate;
  uint16_t blockAlign;
  uint16_t bitsPerSample;
  char     data[4];
  uint32_t dataSize;
};
#pragma pack(pop)

class WavPlayer {
 public:
  // Lists .wav files on the card, most recent first (SD.open enumerates in
  // directory order, which on FAT is creation order - reversing gives
  // newest-first, which is what you want to browse for playback).
  static std::vector<String> listRecordings() {
    std::vector<String> out;
    File root = SD.open("/");
    if (!root) return out;
    File f = root.openNextFile();
    while (f) {
      String name = f.name();
      if (name.endsWith(".wav")) {
        out.push_back(name.startsWith("/") ? name : "/" + name);
      }
      f = root.openNextFile();
    }
    std::reverse(out.begin(), out.end());
    return out;
  }

  // Opens path and locates the "data" sub-chunk. Returns false on a missing
  // file or a header that doesn't match what wav_recorder.h writes.
  bool open(const String &path) {
    close();
    _file = SD.open(path);
    if (!_file) return false;

    WavFileHeader hdr;
    if (_file.read((uint8_t *)&hdr, sizeof(hdr)) != sizeof(hdr) ||
        memcmp(hdr.riff, "RIFF", 4) != 0 ||
        memcmp(hdr.wave, "WAVE", 4) != 0 ||
        memcmp(hdr.data, "data", 4) != 0 ||
        hdr.audioFormat != 1 ||
        hdr.bitsPerSample != 16) {
      _file.close();
      return false;
    }

    _sampleRate = hdr.sampleRate;
    _stereo = hdr.numChannels > 1;
    _dataSize = hdr.dataSize;
    _path = path;
    _playPos = 0;
    _paused = false;
    return true;
  }

  bool isOpen() const { return (bool)_file; }
  bool isPaused() const { return _paused; }
  String path() const { return _path; }

  uint32_t totalSeconds() const {
    uint32_t bytesPerSec = _sampleRate * (_stereo ? 2 : 1) * 2;
    return bytesPerSec > 0 ? _dataSize / bytesPerSec : 0;
  }

  uint32_t elapsedSeconds() const {
    uint32_t bytesPerSec = _sampleRate * (_stereo ? 2 : 1) * 2;
    return bytesPerSec > 0 ? _playPos / bytesPerSec : 0;
  }

  void pause() { _paused = true; }
  void resume() { _paused = false; }

  // Call once per loop() iteration while playing. Pushes one chunk to the
  // speaker's queue when it's ready for more; does nothing while paused,
  // finished, or waiting on the speaker's existing queue.
  void pump() {
    if (!_file || _paused || finished()) return;
    if (M5.Speaker.isPlaying()) return;  // let the current chunk finish

    size_t remaining = _dataSize - _playPos;
    size_t toRead = min(remaining, sizeof(_buf));
    if (toRead == 0) return;

    size_t n = _file.read(_buf, toRead);
    if (n == 0) {
      _playPos = _dataSize;  // treat a read failure as end-of-file
      return;
    }
    _playPos += n;
    M5.Speaker.playRaw((const int16_t *)_buf, n / 2, _sampleRate, _stereo, 1, 0);
  }

  // True once every byte has been handed to the speaker AND the speaker has
  // finished playing it back (not just queued) - otherwise the last chunk's
  // audio gets cut off the instant it's queued.
  bool finished() const {
    return !_file || (_playPos >= _dataSize && !M5.Speaker.isPlaying());
  }

  void close() {
    if (_file) _file.close();
    _dataSize = 0;
    _playPos = 0;
    _paused = false;
  }

 private:
  File _file;
  String _path;
  uint32_t _sampleRate = SAMPLE_RATE_HZ;
  bool _stereo = false;
  uint32_t _dataSize = 0;
  uint32_t _playPos = 0;
  bool _paused = false;
  uint8_t _buf[1024];
};
