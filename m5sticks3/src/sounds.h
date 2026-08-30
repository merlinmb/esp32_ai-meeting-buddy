#pragma once
#include <M5Unified.h>

// Short UI tones played through M5.Speaker (see
// https://docs.m5stack.com/en/arduino/m5sticks3/speaker). All calls are
// fire-and-forget/non-blocking - M5.Speaker queues tones and plays them in
// the background, same as WavPlayer's playRaw() chunks.
namespace Sounds {

// Call once from setup(), after M5.begin(). StickS3's speaker defaults to a
// much lower magnification than other M5 boards, so the library's default
// master volume (64/255) can be nearly inaudible - push it up here.
inline void init() {
  M5.Speaker.setVolume(255);
  bool started = M5.Speaker.tone(1200, 1);  // 1ms - inaudible, just forces begin() now
  Serial.printf("Speaker.tone() returned %d, isPlaying=%d\n", started, M5.Speaker.isPlaying());
}

// Each tone() call defaults to stop_current_sound=true, which cuts off
// whatever's still queued on the same auto-picked channel - back-to-back
// calls need an explicit gap or they can silence each other.
inline void startup() {
  M5.Speaker.tone(1800, 60);
  delay(90);
  M5.Speaker.tone(1800, 60);
}

inline void recordStart() {
  M5.Speaker.tone(2400, 50);
}

inline void recordEnd() {
  M5.Speaker.tone(1600, 50);
}

inline void error() {
  M5.Speaker.tone(400, 200);
}

inline void uploadComplete() {
  M5.Speaker.tone(1500, 70);
  delay(90);
  M5.Speaker.tone(2200, 90);
}

}  // namespace Sounds
