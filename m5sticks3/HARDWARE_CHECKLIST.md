# M5 StickS3 Hardware Verification Checklist

## ✓ Pre-Build Setup

- [ ] Install VS Code with PlatformIO extension
- [ ] Update config.h with your WiFi SSID and password
- [ ] Verify WiFi credentials are correct in config.h
- [ ] Update UPLOAD_SERVER_URL in config.h if needed

## ✓ SD Card Wiring (Per Your Diagram)

Verify these connections with a multimeter before powering on:

```
SD Module Pin  →  M5 StickS3 Pin (GPIO)
─────────────────────────────────────
3V3        →  3V3
GND        →  GND
CS (-2)    →  G7 (GPIO 7)
MOSI (-3)  →  G6 (GPIO 6)
CLK (-4)   →  G5 (GPIO 5)
MISO (-5)  →  G4 (GPIO 4)
```

- [ ] 3V3 power line connected
- [ ] GND connected
- [ ] CS line to GPIO 7
- [ ] MOSI line to GPIO 6
- [ ] CLK line to GPIO 5
- [ ] MISO line to GPIO 4
- [ ] No loose wires or solder bridges
- [ ] SD card inserted in the module

## ✓ Rotary Encoder Wiring (New)

```
Encoder Pin  →  M5 StickS3 (GPIO)
──────────────────────────────
SW (Button)  →  GPIO 0
CLK          →  GPIO 1
DT           →  GPIO 8
GND          →  GND
5V or 3V3    →  3V3 (check encoder specs)
```

- [ ] SW connected to GPIO 0
- [ ] CLK connected to GPIO 1
- [ ] DT connected to GPIO 8
- [ ] GND connected
- [ ] Power line connected

## ✓ Main Button (G11)

- [ ] Firmware configured for GPIO 41 (G11 on M5StickS3)
- [ ] Button responds to presses (test during operation)

## ✓ Battery

- [ ] M5 StickS3 has internal battery
- [ ] Battery voltage display should appear on idle screen (top-right)
- [ ] USB-C cable for charging and programming

## ✓ Build & Upload

```bash
# From the m5sticks3 folder:
cd /path/to/m5sticks3

# Build for M5 StickS3
pio run -e m5sticks3

# Upload to device (device connected via USB)
pio run -e m5sticks3 --target upload

# Monitor serial output
pio device monitor -b 115200
```

- [ ] Code compiles without errors
- [ ] Upload completes successfully
- [ ] Serial monitor shows startup messages

## ✓ Functional Testing

### Display (80x160 LCD)
- [ ] Idle screen shows: time (green), battery %, WiFi status, SD info, last recording
- [ ] Colors render correctly (green, teal, red accents visible)
- [ ] Text is readable (may be small due to 80x160 resolution)

### SD Card
- [ ] SD initialization message shows in serial monitor
- [ ] SD free space percentage displays on idle screen
- [ ] Recording files are created on SD card

### Audio Recording
- [ ] Press main button → recording starts
- [ ] Live waveform displays during recording
- [ ] Elapsed time increments
- [ ] Press button again → recording stops
- [ ] WAV file appears on SD card

### WiFi & Upload
- [ ] Hold main button from idle → menu opens
- [ ] "WiFi info" menu item → shows SSID and connection status
- [ ] "Upload now" → attempts to upload pending recordings
- [ ] Check server logs for successful uploads

### Battery Monitor
- [ ] Battery % displayed in top-right of idle screen
- [ ] Value changes as battery drains (or plug/unplug USB)
- [ ] If readings seem wrong, adjust calibration in readBatteryPercent()

### Rotary Encoder (Optional - requires integration)
- [ ] Currently not integrated into menu navigation
- [ ] Header is ready in src/rotary_encoder.h
- [ ] Can be added to main.cpp's state machine for alternative menu control

## ✓ Troubleshooting

### Audio Issues
- If silent or garbled after everything else works:
  - Check ES8311 codec initialization against M5StickS3 reference firmware
  - Verify I2S pins: BCLK=34, WS=33, DIN=32, DOUT=25
  - Update MCLK/PLL settings in es8311_codec.h

### SD Card Not Detected
- Verify wiring against pinout above
- Try a different SD card (test with known-good card)
- Check SPI.begin() parameters match pins.h
- Inspect solder connections for cold joints

### Battery % Not Displaying
- Verify PIN_BATTERY_ADC is correctly set to 38
- Check if analogRead(38) returns values 0-4095
- Adjust calibration formula in readBatteryPercent()

### WiFi Upload Fails
- Verify WiFi credentials in config.h
- Check if device can reach WiFi network
- Verify UPLOAD_SERVER_URL is accessible
- Test with phone hotspot first

### Main Button Not Responding
- Verify PIN_RECORD_BUTTON is set to 41 (G11)
- Test with `digitalRead(41)` in serial monitor
- Check debounce timing in record_button.h if needed

## ✓ Next Steps

1. **Encoder Integration**: If you want to use the rotary encoder for menu navigation:
   - Include `rotary_encoder.h` in main.cpp
   - Add EncoderEvent handling in the state machine
   - Use encoder LEFT/RIGHT to navigate menu, button to select

2. **Performance Tuning**:
   - Adjust I2S sample rate if needed (currently 16kHz)
   - Fine-tune mic gain (currently 4) in codec.setMicGain()
   - Adjust waveform update rate (currently 120ms per frame)

3. **Custom UI**:
   - M5 StickS3's 80x160 screen is compact; consider different layouts
   - Current UI prioritizes: time, battery, SD status, recording info

## Reference Documentation
- M5 StickS3: https://docs.m5stack.com/en/core/StickS3
- M5Unified: https://github.com/m5stack/M5Unified
- M5GFX: https://github.com/m5stack/M5GFX
- PlatformIO: https://platformio.org/
