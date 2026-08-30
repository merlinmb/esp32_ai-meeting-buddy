#pragma once
#include <Arduino.h>
#include "pins.h"

// Interrupt-driven press detector for a single onboard button (see pins.h
// comments - PWR is a hardware switch and BOOT/RST are reserved for
// flashing, so G11/G12 are the only pins safe to repurpose in application
// code).
//
// With two physical buttons (G11 = select/record, G12 = navigate) there's
// no more need for a timed hold gesture - each button reports a single
// PRESSED event, fired on the press edge. The edge itself is captured by a
// GPIO interrupt (not polled), so a press isn't missed even if loop() is
// busy for a while (SD writes, network calls, etc); poll() just debounces
// and drains that captured edge once per loop() iteration. The same ISR
// wiring is also what lets this pin wake the device from deep sleep later
// (ext0/ext1), which plain polling can't do.

enum class ButtonEvent { NONE, PRESSED };

namespace record_button_detail {
// Interrupts need C-linkage free functions with file-scope state (see
// rotary_encoder.h for why: IRAM_ATTR on inline class member functions can
// trip an l32r relocation error at link time). Two independent buttons need
// two independent ISRs/state slots, so this is templated on pin index
// rather than reusable across instances.
template <int kIndex>
struct ButtonIsrState {
  static volatile bool pressedEdge;
  static volatile unsigned long edgeMs;
  static int pin;
  static bool activeLow;
};
template <int kIndex> volatile bool ButtonIsrState<kIndex>::pressedEdge = false;
template <int kIndex> volatile unsigned long ButtonIsrState<kIndex>::edgeMs = 0;
template <int kIndex> int ButtonIsrState<kIndex>::pin = -1;
template <int kIndex> bool ButtonIsrState<kIndex>::activeLow = true;

template <int kIndex>
void IRAM_ATTR onButtonEdge() {
  using S = ButtonIsrState<kIndex>;
  bool raw = digitalRead(S::pin);
  bool active = S::activeLow ? (raw == LOW) : (raw == HIGH);
  if (active) {
    S::pressedEdge = true;
    S::edgeMs = millis();
  }
}
}  // namespace record_button_detail

template <int kIndex>
class RecordButtonT {
 public:
  RecordButtonT(int pin, bool activeLow) {
    using S = record_button_detail::ButtonIsrState<kIndex>;
    S::pin = pin;
    S::activeLow = activeLow;
  }

  void begin() {
    using namespace record_button_detail;
    using S = ButtonIsrState<kIndex>;
    pinMode(S::pin, S::activeLow ? INPUT_PULLUP : INPUT);
    S::pressedEdge = false;
    attachInterrupt(digitalPinToInterrupt(S::pin), onButtonEdge<kIndex>, S::activeLow ? FALLING : RISING);
  }

  // Call once per loop() iteration.
  ButtonEvent poll() {
    using namespace record_button_detail;
    using S = ButtonIsrState<kIndex>;

    noInterrupts();
    bool edge = S::pressedEdge;
    unsigned long edgeMs = S::edgeMs;
    if (edge) S::pressedEdge = false;
    interrupts();

    if (!edge) return ButtonEvent::NONE;

    // Debounce: ignore an edge that follows too closely on the last
    // accepted one (contact bounce firing several interrupts per press).
    unsigned long now = millis();
    if ((now - _lastAcceptedMs) < kDebounceMs && _lastAcceptedMs != 0) return ButtonEvent::NONE;
    _lastAcceptedMs = edgeMs;
    return ButtonEvent::PRESSED;
  }

 private:
  static const unsigned long kDebounceMs = 30;
  unsigned long _lastAcceptedMs = 0;
};

using RecordButton = RecordButtonT<0>;
using NavButton = RecordButtonT<1>;
