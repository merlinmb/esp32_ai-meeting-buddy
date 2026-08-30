#pragma once
// ============================================================================
// Pin map for the Waveshare ESP32-C6-LCD-1.69
//
// Confirmed against the official schematic (ESP32-C6-LCD-1.69-Schematic.pdf)
// and cross-checked against a working sibling project on the same board
// (github.com/aedile/PELLETINO, an ESP-IDF Pac-Man build). Every value below
// is a real net from the board - nothing here is a placeholder anymore.
//
// This board exposes exactly 8 signal pins on its external header, and
// every one of them already does something onboard:
//   - SCL/SDA (GPIO7/8)      one shared I2C bus for the RTC, the ES8311
//                            audio codec, AND the onboard QMI8658 IMU
//                            (unused by this firmware). Never repurpose
//                            this - it would break audio recording, this
//                            device's whole job.
//   - GPIO9                  the onboard BOOT button: just a passive 10K
//                            pull-up + switch to GND, nothing actively
//                            drives it (confirmed by PELLETINO, which reads
//                            it with a plain digitalRead()-style GPIO
//                            input). Used here as the record button.
//   - GPIO18                 the onboard PWR button - same passive story,
//                            also confirmed by PELLETINO. The board's
//                            actual power-cutoff line is a separate pin
//                            (GPIO15/BAT_EN), not this one, so PWR is just
//                            an ordinary button as far as this firmware is
//                            concerned. Used here as the menu button.
//   - USB_N/USB_P (12/13)    the native USB-CDC serial console pins.
//                            Sacrificed here to give the SD card its last
//                            two SPI signals, since there's no other free
//                            GPIO left on the header - see platformio.ini,
//                            which disables the CDC console accordingly.
//                            Serial.print() becomes a silent no-op;
//                            flashing still works via the BOOT+RESET
//                            button combo (ROM download mode, independent
//                            of this application-level tradeoff).
//   - ESP_TXD/ESP_RXD (16/17) a spare UART pair with no onboard consumer -
//                            this firmware's Serial goes over native
//                            USB-CDC, not this path - so these were the
//                            only genuinely free pins on the board. Used
//                            for the SD card's other two SPI signals.
// ============================================================================

// ---- LCD (ST7789V2, SPI - dedicated pins, not on the external header) -----
#define PIN_LCD_SCLK   1
#define PIN_LCD_MOSI   2
#define PIN_LCD_DC     3
#define PIN_LCD_RST    4
#define PIN_LCD_CS     5
#define PIN_LCD_BL     6
// No LCD MISO - the ST7789 panel is write-only over SPI.

// ---- Audio codec (ES8311: I2S data + I2C control) -------------------------
#define PIN_I2S_MCLK   19
#define PIN_I2S_BCLK   20
#define PIN_I2S_WS     22
#define PIN_I2S_DIN    21  // ADC (mic) data into the ESP32
#define PIN_I2S_DOUT   23  // DAC (speaker) data out of the ESP32

#define PIN_CODEC_I2C_SDA  8
#define PIN_CODEC_I2C_SCL  7
#define ES8311_I2C_ADDR    0x18

// ---- RTC (PCF85063) --------------------------------------------------------
// Confirmed same physical I2C bus as the codec (and the onboard IMU).
#define PIN_RTC_I2C_SDA   PIN_CODEC_I2C_SDA
#define PIN_RTC_I2C_SCL   PIN_CODEC_I2C_SCL
#define PCF85063_I2C_ADDR 0x51

// ---- SD card (SPI, external module wired to the header) --------------------
// Uses the board's only genuinely free pins (16/17) plus the native USB
// D+/D- pins (12/13) - see the header comment above and platformio.ini.
#define PIN_SD_SCLK   16
#define PIN_SD_CS     17
#define PIN_SD_MOSI   12   // USB_N pad
#define PIN_SD_MISO   13   // USB_P pad

// ---- Controls ---------------------------------------------------------------
// The board's two general-purpose buttons - each just a passive 10K
// pull-up + switch to GND (confirmed via schematic and PELLETINO). No
// long-press/short-press disambiguation needed: with two real buttons,
// record and menu each get their own dedicated one instead of overloading
// a single button with press-duration timing. (RST is the third physical
// button on this board, but it's wired to CHIP_EN - a hardware reset line,
// not GPIO-accessible - so it can't be repurposed.)
#define PIN_RECORD_BUTTON  9    // BOOT button
#define PIN_MENU_BUTTON    18   // PWR button
#define RECORD_BUTTON_ACTIVE_LOW  true
#define MENU_BUTTON_ACTIVE_LOW    true
