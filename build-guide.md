# AI Meeting Buddy - Build Guide

Portable meeting recorder built on the Waveshare ESP32-C6-LCD-1.69, with local recording to SD card and an AI transcription pipeline via Claude.

Last updated: 2026-08-28

## 1. Hardware overview

The ESP32-C6-LCD-1.69 already includes most of what this project needs onboard, which simplifies the build a lot versus wiring up separate mic/speaker/battery modules:

| Component | Onboard chip | Notes |
|---|---|---|
| MCU | ESP32-C6 (RISC-V, 160 MHz HP core) | WiFi 6, BLE 5, single core - fine for I2S capture + file writes, don't expect heavy on-device DSP |
| Display | 1.69" 240x280 IPS, ST7789V2 driver | SPI, used for status UI |
| Microphone | Onboard MEMS mic -> ES8311 codec | Codec talks I2S (audio) + I2C (control) to the MCU |
| Speaker output | ES8311 -> NS4150B 3W Class-D amp -> MX1.25 2-pin connector | You wire an external speaker to this connector; there's no onboard speaker driver element itself |
| Battery | MX1.25 2-pin connector, ETA6098 charge IC | 3.7V Li-po plugs straight in - charge/discharge is already handled on-board, nothing extra to wire |
| RTC | PCF85063 | Keeps time across power cycles for timestamping recordings |
| IMU | QMI8658 (6-axis) | Not required for this project, but available if you want shake-to-start or orientation-based UI later |
| Storage | 16MB onboard flash only - **no SD slot** | This is the one thing you have to add yourself |

**Key implication:** since you said the mic and speaker are "built in" and the battery is "wired to the board" - that's accurate, and none of those need new wiring. The only wiring job is the SD card.

## 2. Wiring: SD card module

The board has no SD slot, so you need a small SPI microSD breakout board (the common ones use pins VCC, GND, MISO, MOSI, SCK, CS - roughly $2, e.g. an "Micro SD Card Adapter Module" breakout).

Wire it to the exposed GPIO/SPI pads on the ESP32-C6-LCD-1.69's header:

| SD module pin | Connect to | Notes |
|---|---|---|
| VCC | 3V3 | Do not use 5V - board logic is 3.3V |
| GND | GND | |
| SCK | Shared SPI clock pin (same net as the LCD's SCLK) | SD cards support SPI mode, so you can share the bus with the LCD |
| MOSI | Shared SPI MOSI pin (same net as the LCD's MOSI) | |
| MISO | A free exposed GPIO | The LCD doesn't use MISO (write-only), so this pin needs to be newly wired |
| CS | A separate free exposed GPIO (do NOT share with LCD's CS) | Each SPI device needs its own chip-select |

**Important - exact GPIO numbers:** Waveshare's public docs pages don't publish a full GPIO pinout table, and I don't want to hand you numbers I can't verify against your actual board revision. Before wiring:

1. Open the schematic PDF and pinout diagram linked from the [product wiki page](https://www.waveshare.com/wiki/ESP32-C6-LCD-1.69).
2. Pull the exact pin macros from Waveshare's own demo firmware: [github.com/waveshareteam/ESP32-C6-LCD-1.9](https://github.com/waveshareteam/ESP32-C6-LCD-1.9) (`01_Arduino_Libraries` / `02_Example`) - look for a `pin_config.h` or similar header. That file tells you which GPIOs are already committed to the LCD, codec, IMU, and RTC, and which pads on the header are actually free.
3. Drop the confirmed numbers into `firmware/src/pins.h` (marked with `TODO` in the firmware package) before building.

This matters because getting it wrong risks contending with a pin the codec or LCD already uses.

## 3. System architecture

```
 [Mic] -> ES8311 codec -> I2S -> ESP32-C6 -> WAV frames -> SD card (per-meeting .wav file)
                                     |
                                LCD status UI (recording / idle / battery / upload state)
                                     |
                         WiFi (on-demand, not during recording)
                                     |
                                     v
                   Companion receiver script (your laptop / a small server)
                                     |
                        Speech-to-text pass (see caveat below)
                                     |
                                Claude API (cleanup, summary, action items)
                                     |
                         Saved transcript (.md) and/or emailed
```

Design choices baked into this:

- **Record straight to SD, upload later.** WiFi radio use during I2S capture on a single-core chip risks audio glitches/dropped samples, and burns battery. The firmware finishes writing the WAV file to SD first, then connects to WiFi to upload only when you press "upload" or when idle after a recording ends.
- **The device does not talk to Claude directly.** Claude's API does not accept raw audio for transcription - it's a text-in/text-out (plus image/PDF) model family. So the pipeline needs one more step: something has to turn the WAV into text before Claude ever sees it. That "something" is a speech-to-text engine (see below), not the ESP32 itself - the ESP32 doesn't have the compute for on-device ASR anyway.

## 4. The transcription pipeline (important caveat)

This is worth being explicit about since your project description says "transferring recordings to Claude for processing": Claude cannot transcribe audio directly, today. The realistic pipeline is:

1. **Speech-to-text (ASR):** run the WAV through a dedicated transcription engine. Options, roughly cheapest/most-private to most-managed:
   - **Local:** `whisper.cpp` or `faster-whisper` running on your laptop/server - free, works offline, no data leaves your machine.
   - **Hosted API:** OpenAI's Whisper/transcription API, Deepgram, or AssemblyAI - a few cents per meeting, higher accuracy on noisy audio, needs its own API key.
2. **Claude pass:** feed the raw transcript text to Claude with a prompt to clean up filler words/speaker labels, produce a summary, and pull out action items. This is where Claude adds real value - not the transcription itself, but turning a rough transcript into something usable.
3. **Output:** save the result as a markdown transcript, and/or email it via SMTP.

The companion script (`server/receive_and_transcribe.py`, delivered separately) implements this with local `faster-whisper` by default, since it keeps the whole pipeline free and offline except for the final Claude call - swap in a hosted ASR API by editing one function if you'd rather not run Whisper locally.

## 5. Firmware behavior (ESP32-C6 side)

The board only has one button safe to use in application code - the side button. (PWR is a hardware power switch, not a GPIO; BOOT and RST are reserved for flashing/reset and risky to repurpose.) The UI is built around two gestures on that one button: a short press and a long press (hold ~0.6s).

- **Idle screen:** clock (from the RTC), battery pill, WiFi status dot, and a note when recordings are queued for upload. Short press = start recording. Long press = open the menu.
- **Recording screen:** a live scrolling waveform (real RMS levels computed from the actual audio being captured, not a fake animation), elapsed time, and a red REC indicator. Short or long press = stop and finalize the file (`MEETING_YYYYMMDD_HHMMSS.wav`, timestamped from the RTC; the WAV header is rewritten with the real file size once recording stops, since that's unknown up front).
- **Menu:** short press cycles through Upload now / WiFi info / Storage / About / Exit; long press selects the highlighted item. Auto-returns to idle after 8 seconds of no input, so it can't get stuck open. Storage shows SD used/total space and how many recordings are still waiting to upload; WiFi info shows the configured network and current connection state.
- **Upload:** from the menu, or automatically whenever the device is idle and it's been a while since the last attempt. Connects WiFi, POSTs any un-uploaded WAV files to the receiver, marks each with a `.done` marker on success, then disconnects WiFi to save power.
- **Power:** WiFi radio is only brought up for uploads, never during recording - the single biggest thing you can do for both battery life and recording reliability on a single-core chip.

## 6. What's delivered alongside this doc

- `firmware/` - a PlatformIO project (VS Code + PlatformIO extension): I2S capture, SD/WAV handling, the button gesture + menu state machine, the LCD UI (idle/recording+waveform/menu/info/upload screens), and WiFi upload. `src/pins.h` has placeholders marked `TODO` for the fixed onboard pins you need to pull from Waveshare's schematic/demo repo, plus the SD card pins you're free to choose per section 2.
- `server/receive_and_transcribe.py` - the companion receiver: a Flask server that accepts the WAV upload, transcribes it, sends it to Claude for cleanup/summary, and writes/emails the result.
- `server/Dockerfile`, `docker-compose.yml`, `deploy.sh` - deploys the receiver as a container on **savage.local** (see section 7a) so it's always on and your laptop doesn't need to be.

## 7. Suggested build order

1. Confirm pin numbers from Waveshare's schematic/demo repo, fill in `pins.h`.
2. Wire the SD card breakout per section 2, verify it mounts (a basic SD list-files sketch is the fastest sanity check).
3. Flash the firmware, confirm a short press produces a playable WAV file on the SD card, and that the waveform moves while you talk into the mic.
4. Confirm the menu opens on long-press and Storage/WiFi/About screens render correctly.
5. Deploy the receiver to savage.local (section 7a) or run it locally for a quicker first test, confirm an uploaded WAV shows up and gets transcribed.
6. Case/enclosure, battery capacity, and mounting are up to you at this point - everything electrical is done.

## 7a. Deploying the receiver to savage.local

The receiver is set up to run as a Docker container on a machine called `savage.local`, so transcription doesn't depend on your laptop being on. `server/deploy.sh` handles this using a Docker "SSH context" - your local Docker CLI builds the image by streaming this folder straight to savage.local's Docker daemon over SSH, so there's no registry and no manual file copying involved.

Prerequisites on savage.local: Docker (with the Compose plugin) installed and running, and SSH access from your machine with an account that can run Docker there.

```bash
cd server
cp .env.example .env      # fill in ANTHROPIC_API_KEY at minimum
./deploy.sh setup         # one-time: registers the docker context, verifies savage.local is reachable
./deploy.sh deploy        # builds and starts the container on savage.local
```

`./deploy.sh logs` tails the running container, `./deploy.sh down` stops it, and `./deploy.sh pull-transcripts` copies finished transcripts from savage.local down to your machine (they're stored in a Docker-managed volume there rather than a plain folder - see the comment in `docker-compose.yml` for why). The firmware's `config.h` already points `UPLOAD_SERVER_URL` at `http://savage.local:8787/upload` to match.

If the device can't resolve `savage.local` over mDNS on your network, swap in its plain IP address in `config.h` instead - `ping savage.local` from another machine will tell you what it currently is.

## 8. Open decisions for you

- **ASR choice:** local Whisper (free, private, runs on savage.local) vs. a hosted transcription API (costs money per meeting, less setup, no local compute needed). The delivered script defaults to local `faster-whisper`.
- **Output destination:** save transcripts to the Docker volume (pull them with `deploy.sh pull-transcripts`), email them, or both - the script supports either.
- **Multi-file batching:** if you record several meetings before you're near WiFi, the firmware uploads all pending files in one pass - no changes needed for that case.
- **A second physical button:** the menu currently runs on one button using short/long-press gestures, which works but means everything is sequential (cycle, cycle, cycle, select). Wiring one extra button to a free GPIO later would let you split that into dedicated "next" and "select" buttons if the single-button menu ever feels slow.
