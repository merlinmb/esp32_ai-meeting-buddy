# AI Meeting Buddy - firmware

PlatformIO project for the Waveshare ESP32-C6-LCD-1.69. Open this folder in VS Code with the PlatformIO extension.

## Before building

1. **`src/pins.h`** - replace every `TODO - CONFIRM` value with the real GPIO number from Waveshare's schematic/demo repo (links are in the comments, and in `build-guide.md` section 2). Also pick and wire your two new SD card pins (MISO, CS) per `build-guide.md` section 2.
2. **`src/config.h`** - set your WiFi SSID/password. `UPLOAD_SERVER_URL` already points at `http://savage.local:8787/upload` to match `server/deploy.sh` - change it if you're running the receiver somewhere else.
3. **`src/es8311_codec.h`** - the codec init is a reference sequence (see the comment at the top of that file); if audio is silent or distorted once everything else works, that's the first place to look.

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
3. Short-press the button, watch the LCD switch to the recording screen with a moving waveform, short-press again, then pull the SD card and check a `MEETING_*.wav` file exists and plays. If the waveform never moves, the mic/codec path (not the SD/button/LCD path) is the thing to debug next.
4. Long-press from idle to confirm the menu opens and cycles through Upload now / WiFi info / Storage / About / Exit.
5. Only after 1-4 work, worry about WiFi upload and the codec register tuning.
