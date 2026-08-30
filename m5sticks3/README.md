# AI Meeting Buddy - firmware

PlatformIO project for the M5Stack M5StickS3 (ESP32-S3). Open this folder in VS Code with the PlatformIO extension.

## Before building

1. **`src/config.h`** - set your WiFi SSID/password. `UPLOAD_SERVER_URL` already points at the `savage.local` Docker deployment - change it if you're running the receiver somewhere else.
2. **`src/pins.h`** - already filled in for M5StickS3's pin map (SD card SPI, main button, rotary encoder). Only needed if you change the wiring - display, mic, and codec are handled internally by M5Unified.
3. Wire the SD card per `src/pins.h`: CS/MOSI/CLK/MISO on G7/G6/G5/G4, 3V3/GND to the board's 3V3/GND.

## Build & flash

In VS Code: PlatformIO sidebar -> Build, then Upload. Or from a terminal in this folder:

```
pio run
pio run -t upload
pio device monitor
```

## First smoke test

Before trusting the full pipeline, verify each piece independently:

1. Comment out everything except `recorder.beginSd()` in `setup()` and confirm `SD init failed` does NOT print - confirms wiring.
2. Confirm the LCD shows the "starting up" message - confirms display wiring.
3. Short-press the button, watch the LCD switch to the recording screen with a moving waveform, short-press again, then pull the SD card and check a `MEETING_*.wav` file exists and plays. If the waveform never moves, the mic path (not the SD/button/LCD path) is the thing to debug next.
4. Long-press from idle to confirm the menu opens and cycles through Upload now / WiFi info / Storage / About / Exit.
5. Only after 1-4 work, worry about WiFi upload.

## UI

`src/ui_display.h` is laid out for the StickS3's 135x240 panel: fixed text scales, glyph-based menu icons, text wrapping, and a footer area kept clear of dynamic content. Boot progress is shown via `ui.bootLog(...)` calls during `setup()`.

## Sound feedback

`src/sounds.h` plays short tones through `M5.Speaker` for key events - `Sounds::init()` after `M5.begin()` (also raises master volume, since StickS3 defaults to a much lower speaker level than other M5 boards), `Sounds::startup()` once boot finishes, then `recordStart()` / `recordEnd()` / `error()` / `uploadComplete()` at the matching points in `main.cpp`. No SD card wired in yet - `Sounds::error()` plus an on-screen prompt is the expected result of a recording attempt until the SD adapter is connected.
