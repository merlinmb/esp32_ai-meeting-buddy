#pragma once
// ============================================================================
// Pin map for the Waveshare ESP32-C6-LCD-1.69
//
// IMPORTANT: Waveshare's public docs pages do not publish a full GPIO pinout
// table. The values marked "TODO - CONFIRM" below are placeholders and WILL
// be wrong for some of these signals until you replace them. Before building:
//
//   1. Open the schematic PDF / pinout diagram linked from:
//      https://www.waveshare.com/wiki/ESP32-C6-LCD-1.69
//   2. Pull the real pin macros from Waveshare's own demo firmware:
//      https://github.com/waveshareteam/ESP32-C6-LCD-1.9
//      (look in 01_Arduino_Libraries / 02_Example for a pin_config.h-style
//      header - the LCD, ES8311 codec, IMU, and RTC pins are fixed on the
//      board and defined there.)
//   3. Overwrite every "TODO - CONFIRM" value below with the real number.
//
// The SD_CS / SD_MISO pins are NOT board-fixed - those are yours to choose
// from whatever GPIO pads are exposed and not already claimed by the LCD,
// codec, IMU, or RTC. See build-guide.md section 2 for the wiring approach
// (SD shares SCK/MOSI with the LCD; only CS and MISO are new wires).
// ============================================================================

// ---- LCD (ST7789V2, SPI) --------------------------------------------------
#define PIN_LCD_SCLK   7   // TODO - CONFIRM against schematic
#define PIN_LCD_MOSI   6   // TODO - CONFIRM against schematic
#define PIN_LCD_CS     14  // TODO - CONFIRM against schematic
#define PIN_LCD_DC     15  // TODO - CONFIRM against schematic
#define PIN_LCD_RST    21  // TODO - CONFIRM against schematic
#define PIN_LCD_BL     22  // TODO - CONFIRM against schematic (backlight)

// ---- Audio codec (ES8311: I2S data + I2C control) -------------------------
#define PIN_I2S_MCLK   4   // TODO - CONFIRM against schematic
#define PIN_I2S_BCLK   5   // TODO - CONFIRM against schematic
#define PIN_I2S_WS     8   // TODO - CONFIRM against schematic (LRCK)
#define PIN_I2S_DIN    9   // TODO - CONFIRM against schematic (mic data into ESP32)
#define PIN_I2S_DOUT   10  // TODO - CONFIRM against schematic (speaker data out of ESP32)

#define PIN_CODEC_I2C_SDA  18  // TODO - CONFIRM against schematic (may share a bus with RTC/IMU)
#define PIN_CODEC_I2C_SCL  19  // TODO - CONFIRM against schematic
#define ES8311_I2C_ADDR    0x18

// ---- RTC (PCF85063, I2C - likely same bus as codec) ------------------------
#define PIN_RTC_I2C_SDA   PIN_CODEC_I2C_SDA   // TODO - CONFIRM: same bus, or separate pins?
#define PIN_RTC_I2C_SCL   PIN_CODEC_I2C_SCL
#define PCF85063_I2C_ADDR 0x51

// ---- SD card (SPI, added by you - see build-guide.md section 2) -----------
// SCK/MOSI are shared with the LCD's SPI bus (safe - SD supports SPI mode
// and multiple devices can share SCK/MOSI/MISO as long as each has its own
// CS). MISO is new - the LCD doesn't use it.
#define PIN_SD_SCLK   PIN_LCD_SCLK
#define PIN_SD_MOSI   PIN_LCD_MOSI
#define PIN_SD_MISO   2    // pick any free exposed GPIO pad, then update this
#define PIN_SD_CS     3    // pick any free exposed GPIO pad (must NOT equal PIN_LCD_CS), then update this

// ---- Controls ---------------------------------------------------------------
// The board has PWR / BOOT / RST buttons plus a side button per Waveshare's
// docs. Reusing the side button as record/stop avoids adding a new physical
// button, but confirm which GPIO it's wired to before relying on it - if you'd
// rather wire a dedicated button, pick a free GPIO and wire it to GND with
// INPUT_PULLUP (active-low), same as below.
#define PIN_RECORD_BUTTON  0   // TODO - CONFIRM (side button GPIO) or rewire to a free pad
#define RECORD_BUTTON_ACTIVE_LOW  true
