#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "pins.h"

// ============================================================================
// ES8311 codec bring-up (I2C control side).
//
// This is a REFERENCE init sequence for the ES8311 - a widely used codec on
// ESP32 audio boards - assembled from its public register map. Register
// *addresses* are standard across ES8311 designs; a few *values* (MCLK
// source/divider, PLL settings) depend on how MCLK is wired on this specific
// board, which Waveshare hasn't published.
//
// If audio comes out silent, distorted, or at the wrong pitch/speed after you
// get everything else working: that's almost always an MCLK/PLL mismatch,
// not a wiring problem. The reliable fix is to copy the codec init from
// Waveshare's own Arduino library for this board:
//   https://github.com/waveshareteam/ESP32-C6-LCD-1.9  (01_Arduino_Libraries)
// and drop it in here in place of ES8311::begin()'s register writes - the
// rest of this firmware (recording state machine, SD, LCD, upload) doesn't
// need to change either way.
// ============================================================================

class ES8311 {
 public:
  // Assumes Wire.begin() has already been called for this bus by the caller.
  bool begin(uint32_t sampleRateHz) {
    // Reset
    writeReg(0x00, 0x1F);
    delay(10);
    writeReg(0x00, 0x00);

    // Clock manager: MCLK from pin (not BCLK-derived), enable clocks.
    writeReg(0x01, 0x30);
    writeReg(0x02, 0x00);
    writeReg(0x03, 0x10);
    writeReg(0x16, 0x24);

    // ADC (mic path): format, oversampling.
    writeReg(0x09, 0x00);   // 16-bit I2S, standard format
    writeReg(0x0A, 0x00);
    writeReg(0x17, 0xBF);   // ADC volume close to 0dB

    // DAC (speaker path): format, unmute.
    writeReg(0x0B, 0x00);
    writeReg(0x0C, 0x00);
    writeReg(0x31, 0x00);   // DAC volume close to 0dB
    writeReg(0x32, 0xBF);

    // Power up analog + mic bias, route mic to ADC.
    writeReg(0x0D, 0x01);
    writeReg(0x0E, 0x02);
    writeReg(0x12, 0x00);
    writeReg(0x13, 0x10);
    writeReg(0x1B, 0x0A);
    writeReg(0x1C, 0x6A);

    (void)sampleRateHz;  // sample-rate-dependent PLL tuning: verify against Waveshare's lib if needed
    return true;
  }

  void setMicGain(uint8_t gain0to7) {
    writeReg(0x14, gain0to7 & 0x07);
  }

  void mute(bool m) {
    uint8_t v = readReg(0x31);
    if (m) writeReg(0x31, v | 0x20);
    else writeReg(0x31, v & ~0x20);
  }

 private:
  void writeReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES8311_I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
  }

  uint8_t readReg(uint8_t reg) {
    Wire.beginTransmission(ES8311_I2C_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom((int)ES8311_I2C_ADDR, 1);
    return Wire.available() ? Wire.read() : 0;
  }
};
