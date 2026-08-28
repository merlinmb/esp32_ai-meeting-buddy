#pragma once
#include <Arduino.h>
#include "pins.h"

// Gesture detector for the single onboard side button (see pins.h comments -
// PWR is a hardware switch and BOOT/RST are reserved for flashing, so this
// is the only button safe to repurpose in application code).
//
// Two gestures, no double-click: a menu that needs to feel instant can't
// afford the ~300ms "wait and see if they click again" delay double-click
// detection requires, so short-press always fires immediately on release
// and long-press fires as soon as the hold threshold is crossed (while
// still held, so "hold to open menu" feels responsive too).

enum class ButtonEvent { NONE, SHORT, LONG };

class RecordButton {
 public:
  void begin() {
    pinMode(PIN_RECORD_BUTTON, RECORD_BUTTON_ACTIVE_LOW ? INPUT_PULLUP : INPUT);
  }

  // Call once per loop() iteration.
  ButtonEvent poll() {
    bool raw = digitalRead(PIN_RECORD_BUTTON);
    bool active = RECORD_BUTTON_ACTIVE_LOW ? (raw == LOW) : (raw == HIGH);
    unsigned long now = millis();

    // Debounce: ignore changes until the level's been stable for kDebounceMs.
    if (active != _rawActive) {
      _rawActive = active;
      _lastEdgeMs = now;
      return ButtonEvent::NONE;
    }
    if ((now - _lastEdgeMs) < kDebounceMs) return ButtonEvent::NONE;

    if (active && !_stableActive) {
      // Press just started.
      _stableActive = true;
      _pressStartMs = now;
      _longFired = false;
      return ButtonEvent::NONE;
    }

    if (active && _stableActive && !_longFired && (now - _pressStartMs) >= kLongPressMs) {
      // Still held past the threshold - fire LONG once, immediately.
      _longFired = true;
      return ButtonEvent::LONG;
    }

    if (!active && _stableActive) {
      // Released.
      _stableActive = false;
      bool wasLong = _longFired;
      _longFired = false;
      if (!wasLong) return ButtonEvent::SHORT;  // short click confirmed on release
      return ButtonEvent::NONE;                  // LONG already fired while held
    }

    return ButtonEvent::NONE;
  }

 private:
  static const unsigned long kDebounceMs = 30;
  static const unsigned long kLongPressMs = 600;

  bool _rawActive = false;
  bool _stableActive = false;
  bool _longFired = false;
  unsigned long _lastEdgeMs = 0;
  unsigned long _pressStartMs = 0;
};
