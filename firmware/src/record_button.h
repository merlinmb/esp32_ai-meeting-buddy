#pragma once
#include <Arduino.h>

// This board has two general-purpose buttons (BOOT and PWR - see pins.h),
// each just a passive pull-up + switch to GND. With two real buttons
// available, record and menu each get their own dedicated one instead of
// overloading a single button with short/long-press timing - so this is
// just a plain debounced "was this button just pressed" edge detector,
// used twice (once per button) from main.cpp.

class DebouncedButton {
 public:
  void begin(int pin, bool activeLow) {
    _pin = pin;
    _activeLow = activeLow;
    pinMode(_pin, activeLow ? INPUT_PULLUP : INPUT);
  }

  // Call once per loop() iteration. Returns true exactly once per physical
  // press (on the press edge, once the level's been stable for
  // kDebounceMs), not on release.
  bool pressed() {
    bool raw = digitalRead(_pin);
    bool active = _activeLow ? (raw == LOW) : (raw == HIGH);
    unsigned long now = millis();

    if (active != _rawActive) {
      _rawActive = active;
      _lastEdgeMs = now;
      return false;
    }
    if ((now - _lastEdgeMs) < kDebounceMs) return false;

    if (active && !_stableActive) {
      _stableActive = true;
      return true;
    }
    if (!active && _stableActive) {
      _stableActive = false;
    }
    return false;
  }

 private:
  static const unsigned long kDebounceMs = 30;

  int _pin = -1;
  bool _activeLow = true;
  bool _rawActive = false;
  bool _stableActive = false;
  unsigned long _lastEdgeMs = 0;
};
