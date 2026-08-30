#pragma once
#include <Arduino.h>
#include <math.h>

// Feeds raw 16-bit PCM chunks in as they're captured, computes a per-chunk
// RMS level, and keeps a scrolling ring buffer of levels for the recording
// screen's waveform bars. Cheap enough to run every loop() iteration
// alongside the actual SD write - it doesn't touch the audio data itself,
// just reads it for the level calculation.

class AudioVisualizer {
 public:
  static const int kBarCount = 48;  // how many bars the waveform view draws

  void reset() {
    for (int i = 0; i < kBarCount; i++) _levels[i] = 0;
    _writeIdx = 0;
  }

  // buf/len are the raw bytes just written to the WAV file (16-bit PCM).
  void feed(const uint8_t *buf, size_t len) {
    const int16_t *samples = (const int16_t *)buf;
    size_t count = len / sizeof(int16_t);
    if (count == 0) return;

    double sumSquares = 0;
    for (size_t i = 0; i < count; i++) {
      double s = samples[i];
      sumSquares += s * s;
    }
    double rms = sqrt(sumSquares / count);

    // Map RMS (0..~32768) to a 0..255 bar height with a bit of headroom so
    // normal speech doesn't just peg every bar at max.
    uint8_t level = (uint8_t)constrain((rms / 8000.0) * 255.0, 0.0, 255.0);

    _levels[_writeIdx] = level;
    _writeIdx = (_writeIdx + 1) % kBarCount;
  }

  // Copies the ring buffer out in chronological (oldest-to-newest) order,
  // ready to draw left-to-right.
  void snapshot(uint8_t out[kBarCount]) const {
    for (int i = 0; i < kBarCount; i++) {
      out[i] = _levels[(_writeIdx + i) % kBarCount];
    }
  }

 private:
  uint8_t _levels[kBarCount] = {0};
  int _writeIdx = 0;
};
