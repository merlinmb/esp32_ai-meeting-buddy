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

The pin numbers below are confirmed against the official schematic (`ESP32-C6-LCD-1.69-Schematic.pdf`, from the product wiki page) and cross-checked against a working sibling project on the same board - no more guessing. This board's external header only exposes 8 signal pins total, and every one of them already does *something* onboard (the LCD, the shared RTC/codec/IMU I2C bus, the two buttons, or native USB serial), so there's no way to wire a 4-wire SPI SD card without sacrificing one of those. This project sacrifices the native USB-CDC serial console (see `firmware/platformio.ini`) rather than the audio codec or a button, since losing `Serial.print()` output is an inconvenience, not a broken core feature:

| SD module pin | ESP32-C6 GPIO | Header label | Notes |
|---|---|---|---|
| VCC | 3V3 | 3V3 | Do not use 5V - board logic is 3.3V |
| GND | GND | GND | |
| SCK | GPIO16 | ESP_TXD | Genuinely free - unused by this firmware |
| CS | GPIO17 | ESP_RXD | Genuinely free - unused by this firmware |
| MOSI | GPIO12 | USB_N | Native USB D- pad - repurposed since CDC serial is disabled |
| MISO | GPIO13 | USB_P | Native USB D+ pad - repurposed since CDC serial is disabled |

None of these overlap with the LCD (GPIO1-6, not exposed on the header at all) or the shared RTC/codec/IMU I2C bus (SCL=GPIO7/SDA=GPIO8) - repurposing SCL/SDA for the SD card would have broken the audio codec, which is this device's whole job, so that was ruled out first.

**If you want to double-check any of this yourself:** `docs.waveshare.com`/`www.waveshare.com`/`files.waveshare.com` may be unreachable from some sandboxed dev environments (an egress-proxy policy issue, not a real network problem) - if so, download the schematic PDF yourself and open it locally, or read it with any AI assistant that can process PDF/image attachments directly. [github.com/aedile/PELLETINO](https://github.com/aedile/PELLETINO)'s `docs/HARDWARE.md` is also a solid independent cross-check - a different project on the exact same board.

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

The board has two general-purpose buttons usable in application code: BOOT (GPIO9) and PWR (GPIO18) - both just a passive pull-up + switch to GND, confirmed safe to repurpose via the schematic and the PELLETINO cross-check in section 2. (RST is the third physical button, but it's wired to the chip's hardware reset line, not a GPIO, so it can't be repurposed.) BOOT is the record button; PWR is the menu button - no short/long-press timing needed, since each gesture gets its own dedicated button.

- **Idle screen:** clock (from the RTC), WiFi status dot, SD free-space ring, and last-recording card. BOOT = start recording. PWR = open the menu.
- **Recording screen:** a live scrolling waveform (real RMS levels computed from the actual audio being captured, not a fake animation), elapsed time, and a red REC indicator. BOOT = stop and finalize the file (`MEETING_YYYYMMDD_HHMMSS.wav`, timestamped from the RTC; the WAV header is rewritten with the real file size once recording stops, since that's unknown up front).
- **Menu:** BOOT cycles through Upload now / WiFi info / Storage / About / Exit; PWR selects the highlighted item. Auto-returns to idle after 8 seconds of no input, so it can't get stuck open. Storage shows SD used/total space and how many recordings are still waiting to upload; WiFi info shows the configured network and current connection state.
- **Upload:** runs on its own background task (not inline in the main loop), so it never delays button response or LCD redraws even mid-upload. Attempted automatically about every 5 minutes whenever nothing is recording, or immediately via the menu's "Upload now". Pressing BOOT to start recording interrupts an in-flight upload within about one network chunk - recording always wins.
- **Power:** WiFi radio is only brought up for uploads, never during recording - the single biggest thing you can do for both battery life and recording reliability.

## 6. What's delivered alongside this doc

- `firmware/` - a PlatformIO project (VS Code + PlatformIO extension): I2S capture, SD/WAV handling, the two-button + menu state machine, the LCD UI (idle/recording+waveform/menu/info/upload screens), and a background-task WiFi uploader. `src/pins.h` has the confirmed real GPIO numbers for everything, including the SD card wiring from section 2 - nothing left to fill in.
- `server/receive_and_transcribe.py` - the companion receiver: a Flask server that accepts the WAV upload, transcribes it, sends it to Claude for cleanup/summary, and writes/emails the result.
- `server/Dockerfile`, `docker-compose.yml`, `deploy.sh` - deploys the receiver as a container on **savage.local** (see section 7a) so it's always on and your laptop doesn't need to be.

## 7. Suggested build order

1. Wire the SD card breakout per section 2, verify it mounts (a basic SD list-files sketch is the fastest sanity check).
2. Flash the firmware (note: with the native USB-CDC console disabled per section 2, you won't see `Serial.print()` output over USB - flashing itself still works via the BOOT+RESET button combo). Confirm BOOT produces a playable WAV file on the SD card, and that the waveform moves while you talk into the mic.
3. Confirm the menu opens on PWR and Storage/WiFi/About screens render correctly.
4. Deploy the receiver to savage.local (section 7a) or run it locally for a quicker first test, confirm an uploaded WAV shows up and gets transcribed.
5. Case/enclosure, battery capacity, and mounting are up to you at this point - everything electrical is done.

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
- **Multi-file batching:** if you record several meetings before you're near WiFi, the upload worker sends all pending files in one pass - no changes needed for that case.
- **No USB serial console:** since the native USB D+/D- pads now drive the SD card's MOSI/MISO, there's no live `Serial.print()` debug output during normal operation. If you need it back later, that means giving up either the SD card wiring in section 2 or (worse) the onboard I2C bus that the audio codec/RTC depend on - there's no free pin left to add a console back without a tradeoff somewhere.
