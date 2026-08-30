#pragma once
// ============================================================================
// Pin map for M5 StickS3
// ============================================================================

// ---- Display (M5Unified/M5GFX handles automatically) ---------------------
// M5StickS3 has 80x160 display, controlled via M5Unified

// ---- Audio ------------------------------------------------------------
// Mic (I2S + ES8311 codec) is handled by M5Unified's M5.Mic - it knows
// the board-correct pins/PLL config for StickS3 internally.

// ---- Clock --------------------------------------------------------------
// No RTC chip on this board - wall-clock time comes from NTP (see
// ntp_clock.h), synced over WiFi at boot.

// ---- SD Card (SPI) --------------------------------------------------------
// Per your wiring diagram:
// SD 3V3 -> M5StickS3 3V3, SD GND -> GND
#define PIN_SD_SCLK   5   // G5 (CLK from diagram)
#define PIN_SD_MOSI   6   // G6 (MOSI from diagram)
#define PIN_SD_MISO   4   // G4 (MISO from diagram)
#define PIN_SD_CS     7   // G7 (CS from diagram)

// ---- Controls: Main Button & Encoder ------------------------------------
// GPIO41 is the LCD's SPI chip-select pin (see M5Stack's StickS3 pinmap) -
// using it as a button pin steals CS away from the display driver, which
// then can never update the panel again. KEY1 (GPIO11) is the correct pin.
#define PIN_RECORD_BUTTON  11  // KEY1 on M5StickS3
#define RECORD_BUTTON_ACTIVE_LOW  true

// Pushbutton rotary encoder
#define ENCODER_SW   0   // Switch/Button
#define ENCODER_CLK  1   // Clock
#define ENCODER_DT   8   // Data


