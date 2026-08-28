#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "pins.h"

// Minimal driver for the PCF85063 RTC (NXP part, public datasheet register
// map - this one is not board-specific, unlike the codec/LCD pins).

struct RtcTime {
  int year;    // full year, e.g. 2026
  int month;   // 1-12
  int day;     // 1-31
  int hour;    // 0-23
  int minute;  // 0-59
  int second;  // 0-59
};

class Pcf85063 {
 public:
  // Assumes Wire.begin() has already been called for this bus (shared with
  // the codec - see pins.h) by the time this runs.
  bool begin() {
    // Make sure oscillator is running / not stopped (register 0x00, bit 5 = STOP).
    writeReg(0x00, 0x00);
    return true;
  }

  bool getTime(RtcTime &out) {
    Wire.beginTransmission(PCF85063_I2C_ADDR);
    Wire.write(0x04);  // seconds register
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)PCF85063_I2C_ADDR, 7) != 7) return false;

    uint8_t sec   = Wire.read();
    uint8_t min_  = Wire.read();
    uint8_t hour  = Wire.read();
    uint8_t day   = Wire.read();
    Wire.read();  // weekday, unused
    uint8_t month = Wire.read();
    uint8_t year  = Wire.read();

    out.second = bcdToDec(sec & 0x7F);
    out.minute = bcdToDec(min_ & 0x7F);
    out.hour   = bcdToDec(hour & 0x3F);
    out.day    = bcdToDec(day & 0x3F);
    out.month  = bcdToDec(month & 0x1F);
    out.year   = 2000 + bcdToDec(year);
    return true;
  }

  bool setTime(const RtcTime &t) {
    Wire.beginTransmission(PCF85063_I2C_ADDR);
    Wire.write(0x04);
    Wire.write(decToBcd(t.second));
    Wire.write(decToBcd(t.minute));
    Wire.write(decToBcd(t.hour));
    Wire.write(decToBcd(t.day));
    Wire.write(0x00);  // weekday, not tracked
    Wire.write(decToBcd(t.month));
    Wire.write(decToBcd(t.year - 2000));
    return Wire.endTransmission() == 0;
  }

  // Formats as MEETING_YYYYMMDD_HHMMSS for filenames.
  String filenameTimestamp() {
    RtcTime t;
    if (!getTime(t)) return "MEETING_UNKNOWN_TIME";
    char buf[32];
    snprintf(buf, sizeof(buf), "MEETING_%04d%02d%02d_%02d%02d%02d",
             t.year, t.month, t.day, t.hour, t.minute, t.second);
    return String(buf);
  }

 private:
  static uint8_t bcdToDec(uint8_t v) { return ((v / 16) * 10) + (v % 16); }
  static uint8_t decToBcd(uint8_t v) { return ((v / 10) * 16) + (v % 10); }

  void writeReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(PCF85063_I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
  }
};
