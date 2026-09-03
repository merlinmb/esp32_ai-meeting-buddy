#include "Arduino.h"
#include "climate.h"

extern "C" {
#include "../../src/i2c_bsp/i2c_bsp.h"
}

// SHTC3 commands (big-endian on the wire)
static const uint8_t SHTC3_CMD_WAKE[2]    = { 0x35, 0x17 };
static const uint8_t SHTC3_CMD_MEASURE[2] = { 0x7C, 0xA2 }; // normal mode, T first, clock stretching disabled
static const uint8_t SHTC3_CMD_SLEEP[2]   = { 0xB0, 0x98 };

// Board self-heating (ESP32 + regulators near the sensor) biases raw
// readings high versus ambient - measured offset, subtracted below.
static const float TEMP_OFFSET_C = -8.0f;

bool climateRead(float* tempC, float* humidityPct) {
  if (!shtc3_handle) return false;

  i2c_write_buff(shtc3_handle, -1, (uint8_t*)SHTC3_CMD_WAKE, 2);
  delay(1);

  if (i2c_write_buff(shtc3_handle, -1, (uint8_t*)SHTC3_CMD_MEASURE, 2) != 0) return false;
  delay(15);

  uint8_t data[6] = {0};
  if (i2c_read_buff(shtc3_handle, -1, data, 6) != 0) return false;

  i2c_write_buff(shtc3_handle, -1, (uint8_t*)SHTC3_CMD_SLEEP, 2);

  uint16_t rawT = ((uint16_t)data[0] << 8) | data[1];
  uint16_t rawH = ((uint16_t)data[3] << 8) | data[4];

  if (tempC)       *tempC       = -45.0f + 175.0f * ((float)rawT / 65535.0f) + TEMP_OFFSET_C;
  if (humidityPct) *humidityPct = 100.0f * ((float)rawH / 65535.0f);
  return true;
}
