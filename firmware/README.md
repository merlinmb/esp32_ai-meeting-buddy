# AI Meeting Buddy - firmware

PlatformIO project for the Waveshare ESP32-C6-LCD-1.69. Open this folder in VS Code with the PlatformIO extension.

## Before building

1. **`src/pins.h`** - already filled in with the real, schematic-confirmed GPIO numbers (see `build-guide.md` section 2 for how they were verified and why the SD card ended up wired the way it is). Nothing to edit here unless your build deviates from the wiring in section 2.
2. **`src/config.h`** - set your WiFi SSID/password. `UPLOAD_SERVER_URL` already points at `http://savage.local:8787/upload` to match `server/deploy.sh` - change it if you're running the receiver somewhere else.
3. **`src/es8311_codec.h`** - the codec init is a reference sequence (see the comment at the top of that file); if audio is silent or distorted once everything else works, that's the first place to look.

## Build & flash

In VS Code: PlatformIO sidebar -> Build, then Upload. Or from a terminal in this folder:

```
pio run
pio run -t upload
```

Note: `pio device monitor` won't show anything. The native USB D+/D- pins now drive the SD card's MOSI/MISO (see `pins.h` and `platformio.ini`), so the USB-CDC serial console is disabled - `Serial.print()` calls are silent no-ops. Flashing itself is unaffected (it goes through the chip's ROM-level download mode via the BOOT+RESET button combo, independent of this app-level tradeoff); you just don't get a live log. Use the LCD screens below for bring-up feedback instead.

## First smoke test

Since there's no serial console, bring-up verification relies on the LCD and the SD card's contents rather than log output:

1. Confirm the LCD shows the "starting up" message - confirms display wiring (and, incidentally, that the LCD's own SPI peripheral is fine independent of the SD card's).
2. From the idle screen, check the SD free-space ring: a solid ring with a percentage means the card mounted; "NO SD CARD" means it didn't - confirms SD wiring without needing serial output.
3. Press BOOT, watch the LCD switch to the recording screen with a moving waveform, press BOOT again, then pull the SD card and check a `MEETING_*.wav` file exists and plays. If the waveform never moves, the mic/codec path (not the SD/button/LCD path) is the thing to debug next.
4. Press PWR from idle to confirm the menu opens and cycles through Upload now / WiFi info / Storage / About / Exit (BOOT cycles items, PWR selects).
5. Only after 1-4 work, worry about WiFi upload and the codec register tuning.
