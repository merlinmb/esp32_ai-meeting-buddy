#pragma once
#include <Arduino.h>
#include "pins.h"

// Handles pushbutton rotary encoder for menu navigation.
// Can detect: button press, rotation left (CCW), rotation right (CW).
//
// Rotation is decoded in a CHANGE interrupt on CLK/DT using Ben Buxton's
// published half-step quadrature table (from his widely-deployed "rotary"
// Arduino library) rather than a hand-rolled state machine: an earlier
// stricter full-step table that aborted back to START on any
// out-of-sequence pin reading turned out to be too fragile for this
// hardware's real bounce pattern (it made most turns register nothing at
// all). Buxton's table is tolerant of noisy/partial transitions because it
// only requires forward progress relative to the *previous* state, not a
// perfect textbook sequence, which is why it's the reference decoder many
// working Arduino rotary encoder projects use. It's called "half-step"
// because it counts two steps per mechanical detent (once passing the
// midpoint, once returning to rest) instead of one - poll() below divides
// back down to one LEFT/RIGHT per detent.
//
// The ISR is a plain free function (not a class member) operating on
// file-scope volatile state: ESP32 Arduino's IRAM_ATTR on C++ member
// functions defined inline in a header can trip the toolchain's "literal
// placed after use" l32r relocation error at link time, since the
// per-member-function COMDAT section doesn't keep its literal pool inside
// IRAM range. A free function avoids that class of bug entirely, which is
// also why virtually every ESP32 encoder library does it this way.

enum class EncoderEvent { NONE, BUTTON, LEFT, RIGHT };

namespace rotary_encoder_detail {

enum TableState : uint8_t {
  kStart = 0x0,
  kCcwBegin = 0x1,
  kCwBegin = 0x2,
  kStartM = 0x3,
  kCwBeginM = 0x4,
  kCcwBeginM = 0x5,
};
static const uint8_t kCwFlag = 0x10;
static const uint8_t kCcwFlag = 0x20;
static const uint8_t kStateMask = 0x0F;

// Buxton's half_step_table, indexed [state][pins] with
// pins = (CLK<<1 | DT) in {0,1,2,3}. Transcribed verbatim from the
// published reference table in his rotary encoder library.
static const uint8_t kStateTable[6][4] = {
    /* kStart     */ {kStartM,               kCwBegin,   kCcwBegin,  kStart},
    /* kCcwBegin  */ {kStartM | kCcwFlag,    kStart,     kCcwBegin,  kStart},
    /* kCwBegin   */ {kStartM | kCwFlag,     kCwBegin,   kStart,     kStart},
    /* kStartM    */ {kStartM,               kCcwBeginM, kCwBeginM,  kStart},
    /* kCwBeginM  */ {kStartM,               kStartM,    kCwBeginM,  kStart | kCwFlag},
    /* kCcwBeginM */ {kStartM,               kCcwBeginM, kStartM,    kStart | kCcwFlag},
};

static volatile uint8_t g_tableState = kStart;
static volatile int8_t g_pendingHalfSteps = 0;  // +1 per CW half-step, -1 per CCW

inline uint8_t readQuadratureState() {
  return (digitalRead(ENCODER_CLK) << 1) | digitalRead(ENCODER_DT);
}

void IRAM_ATTR onEncoderEdge() {
  uint8_t pins = readQuadratureState();
  uint8_t next = kStateTable[g_tableState][pins];
  if (next & kCwFlag) g_pendingHalfSteps++;
  else if (next & kCcwFlag) g_pendingHalfSteps--;
  g_tableState = next & kStateMask;
}

}  // namespace rotary_encoder_detail

class RotaryEncoder {
 public:
  void begin() {
    pinMode(ENCODER_SW, INPUT_PULLUP);
    pinMode(ENCODER_CLK, INPUT_PULLUP);
    pinMode(ENCODER_DT, INPUT_PULLUP);

    using namespace rotary_encoder_detail;
    g_tableState = kStart;
    attachInterrupt(digitalPinToInterrupt(ENCODER_CLK), onEncoderEdge, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ENCODER_DT), onEncoderEdge, CHANGE);
  }

  // Call once per loop() iteration.
  EncoderEvent poll() {
    // Button press (active-low), time-based debounce.
    bool raw = (digitalRead(ENCODER_SW) == LOW);
    unsigned long now = millis();
    if (raw != _rawActive) {
      _rawActive = raw;
      _lastEdgeMs = now;
    } else if ((now - _lastEdgeMs) >= kDebounceMs && raw != _stableActive) {
      _stableActive = raw;
      if (_stableActive) return EncoderEvent::BUTTON;  // fires on press
    }

    // Rotation: Buxton's table counts 2 half-steps per mechanical detent
    // (see kStateTable comment), so a full click needs |pending| >= 2.
    // Reset to 0 (not just -=2) on fire so a stray leftover half-step from
    // an incomplete/bounced turn never has to be unwound before the
    // opposite direction can register - carrying a leftover forward is
    // exactly what caused the "takes 2 clicks to reverse" bug previously.
    using namespace rotary_encoder_detail;
    noInterrupts();
    int8_t pending = g_pendingHalfSteps;
    if (pending >= 2 || pending <= -2) g_pendingHalfSteps = 0;
    interrupts();

    if (pending >= 2) return EncoderEvent::RIGHT;
    if (pending <= -2) return EncoderEvent::LEFT;
    return EncoderEvent::NONE;
  }

 private:
  static const unsigned long kDebounceMs = 10;

  bool _rawActive = false;
  bool _stableActive = false;
  unsigned long _lastEdgeMs = 0;
};
