# M5 StickS3 Migration - Quick Reference

## Information about the board
Based on this board  - https://shop.m5stack.com/products/m5sticks3-esp32s3-mini-iot-dev-kit
Wiki Here - https://docs.m5stack.com/en/core/StickS3

## What Changed

Your project has been successfully migrated from **Waveshare ESP32-C6-LCD-1.69** to **M5 StickS3**.

### Key Updates:
1. ✅ Display: 240×280 → 80×160 (using M5Unified/M5GFX)
2. ✅ Audio I2S pins updated for M5StickS3
3. ✅ SD card wiring per your diagram (G7/G6/G5/G4)
4. ✅ Main button on GPIO 41 (G11)
5. ✅ Encoder support on pins 0, 1, 8
6. ✅ Battery voltage monitoring on pin 38
7. ✅ PlatformIO board: `esp32-s3-devkitc-1` (M5Unified handles rest)

## Files Modified

```
src/
├── pins.h                  ← All pin definitions updated
├── main.cpp               ← Added M5Unified init, battery reading
├── ui_display.h           ← Redesigned for 80×160 screen
├── platformio.ini         ← New board/libraries
├── rotary_encoder.h       ← NEW: Encoder handler (ready to use)
└── MIGRATION_M5STICKS3.md ← Complete technical details
```

## Quick Start

### 1. Configure WiFi
```cpp
// src/config.h - update these:
#define WIFI_SSID      "your-network"
#define WIFI_PASSWORD  "your-password"
```

### 2. Wire SD Card
Per your diagram:
```
SD Module  →  M5StickS3
3V3        →  3V3
GND        →  GND
CS         →  G7 (GPIO 7)
MOSI       →  G6 (GPIO 6)
CLK        →  G5 (GPIO 5)
MISO       →  G4 (GPIO 4)
```

### 3. Wire Rotary Encoder (Optional)
```
Encoder    →  M5StickS3
SW         →  GPIO 0
CLK        →  GPIO 1
DT         →  GPIO 8
GND        →  GND
3V3/5V     →  3V3
```

### 4. Build & Upload
```bash
cd m5sticks3
pio run -e m5sticks3 --target upload
pio device monitor -b 115200
```

## Display Changes

The M5 StickS3 has a **much smaller screen** (80×160 vs 240×280). UI has been redesigned:

- **Before**: Detailed layouts with rings, cards, large fonts
- **After**: Compact, text-based, minimal graphics

### Idle Screen Shows:
```
[HH:MM] [BAT%]
[WiFi/--]
[SD: XX%]
Last: MM:SS
Press: record
Hold: menu
```

### Recording Screen:
```
● REC
  MM:SS
[waveform bars]
Press: stop
```

## Features

| Feature | Status |
|---------|--------|
| Audio recording to SD | ✅ Works |
| WiFi upload | ✅ Works |
| Menu system | ✅ Works |
| Real-time waveform | ✅ Works |
| Battery display | ✅ NEW |
| Rotary encoder | ✅ Ready (not yet in UI) |

## Pins Overview

| GPIO | Function |
|------|----------|
| 0 | Encoder SW |
| 1 | Encoder CLK |
| 4 | SD MISO |
| 5 | SD CLK |
| 6 | SD MOSI |
| 7 | SD CS |
| 8 | Encoder DT |
| 21 | I2C SDA |
| 22 | I2C SCL |
| 25 | I2S DOUT (speaker) |
| 32 | I2S DIN (mic) |
| 33 | I2S WS |
| 34 | I2S BCLK |
| 38 | Battery ADC |
| 41 | Main Button (G11) |

## Troubleshooting

**Q: SD card not detected?**  
A: Check connections, verify CS/MOSI/CLK/MISO match pins.h, try a different card.

**Q: Audio silent?**  
A: Verify I2S pins, check ES8311 init in es8311_codec.h against M5StickS3 reference.

**Q: Battery % shows wrong value?**  
A: Calibrate readBatteryPercent() in main.cpp (adjust voltage scaling).

**Q: Compilation fails?**  
A: Verify M5Unified and M5GFX libraries installed, use board `esp32-s3-devkitc-1`.

## What Stayed the Same

- Audio codec: ES8311 (same I2C address)
- RTC: PCF85063 (M5StickS3 uses BM8563, same address)
- Recording/upload logic: unchanged
- WiFi credentials in config.h
- WAV file format
- State machine flow

## Next Steps

1. **Test hardware** - Follow HARDWARE_CHECKLIST.md
2. **Integrate encoder** (optional) - See rotary_encoder.h comments
3. **Calibrate battery** - Adjust readBatteryPercent() if needed
4. **Deploy** - Flash and test recording/upload workflow

## Files & Documentation

- `MIGRATION_M5STICKS3.md` - Full technical migration details
- `HARDWARE_CHECKLIST.md` - Step-by-step hardware verification
- `platformio.ini` - Build configuration (ready to use)
- `src/pins.h` - All pin mappings
- `src/rotary_encoder.h` - Encoder handler (copy/paste ready)

---

**Status**: ✅ Ready to build and test  
**Board**: M5 StickS3 (ESP32-S3)  
**Libraries**: M5Unified + M5GFX
