# AI Meeting Buddy

A portable, voice-controlled meeting recorder built on the Waveshare ESP32-C6-LCD-1.69 microcontroller. Record meetings locally to SD card, then transcribe and process them through Claude's API for cleanup, summarization, and action item extraction.

## Project Overview

The AI Meeting Buddy combines embedded recording hardware with a companion transcription pipeline to capture, transcribe, and intelligently summarize meetings. Everything you record stays on your device until explicitly uploaded, and the transcription pipeline runs locally or on your own infrastructure—keeping your meeting data private.

### Key Features

- **Single-button gesture control:** short press to record, long press for menu access
- **Live waveform visualization:** see actual audio levels on the 1.69" display while recording
- **Local storage:** all meetings saved to microSD card before upload
- **Offline-capable:** transcription can run entirely on your own infrastructure with local Whisper
- **Claude integration:** automatic cleanup, summarization, and action item extraction
- **Battery-optimized:** WiFi radio only active during uploads, not during recording
- **Timestamped recordings:** RTC keeps accurate time across power cycles
- **Upload queue:** multiple meetings batch-uploaded in a single WiFi session

## What's Inside

```
├── firmware/              # ESP32-C6 embedded firmware (PlatformIO)
│   ├── src/
│   │   ├── main.cpp       # Button gestures, state machine, UI screens
│   │   ├── pins.h         # GPIO configuration (TODO: fill in for your board)
│   │   ├── config.h       # WiFi & upload server settings
│   │   ├── i2s_capture.h  # Microphone capture via I2S
│   │   ├── sd_wav.h       # WAV file recording to SD card
│   │   └── es8311_codec.h # Audio codec initialization
│   ├── platformio.ini     # Build configuration
│   └── README.md          # Firmware-specific build instructions
│
├── server/                # Transcription & Claude pipeline
│   ├── receive_and_transcribe.py  # Upload receiver + transcription + Claude processing
│   ├── docker-compose.yml         # Container orchestration
│   ├── deploy.sh                  # SSH-based deployment to remote Docker host
│   ├── Dockerfile                 # Container image definition
│   ├── requirements.txt            # Python dependencies
│   ├── .env.example               # Environment variables template
│   └── README.md                  # Receiver deployment & configuration
│
├── build-guide.md         # Detailed hardware, wiring, architecture, and build steps
└── README.md              # This file
```

## Quick Start

### Prerequisites

- **Hardware:** Waveshare ESP32-C6-LCD-1.69, microSD card breakout board, microSD card, 3.7V Li-Po battery
- **Tools:** VS Code with PlatformIO extension (for firmware); Python 3.10+ and Docker (for server)
- **API access:** Anthropic API key for Claude integration

### 1. Prepare the Firmware

See [`firmware/README.md`](firmware/README.md) for detailed setup instructions:

1. Confirm GPIO pin numbers from Waveshare's schematic and populate `firmware/src/pins.h`
2. Wire the microSD card breakout board per [`build-guide.md` section 2](build-guide.md#2-wiring-sd-card-module)
3. Set WiFi credentials in `firmware/src/config.h`
4. Build and flash: `pio run && pio run -t upload`
5. Run smoke tests (see firmware README section "First smoke test")

### 2. Deploy the Transcription Server

See [`server/README.md`](server/README.md) for deployment options. The quickest start:

```bash
cd server
cp .env.example .env
# Edit .env and set ANTHROPIC_API_KEY and other settings
docker-compose up
```

The server listens on `http://localhost:8787/upload` (or wherever you deploy it). Update `firmware/src/config.h` `UPLOAD_SERVER_URL` to match.

For always-on operation, deploy to a remote Docker host via SSH:

```bash
cd server
./deploy.sh setup         # First time: register the remote Docker context
./deploy.sh deploy        # Build and start the container
./deploy.sh logs          # Monitor logs
```

### 3. Test the Full Pipeline

1. Record a short test meeting on the device (press button to start/stop)
2. From the device menu, select "Upload now"
3. Check the server logs for transcription and Claude processing
4. Retrieved the processed transcript from the server output location

## System Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    AI Meeting Buddy Device                      │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │  [Microphone]                                              │ │
│  │       ↓                                                     │ │
│  │  [ES8311 Codec]  ←→ I2C/I2S protocol                      │ │
│  │       ↓                                                     │ │
│  │  [ESP32-C6] → WAV frames → [SD Card]                      │ │
│  │       ↓ (on idle/button press)                             │ │
│  │  [WiFi] → POST /upload/                                    │ │
│  │       ↓                                                     │ │
│  │  [LCD UI] (status, recording, menu, battery, WiFi state)   │ │
│  └────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
                              ↓
                    [Network / Internet]
                              ↓
┌─────────────────────────────────────────────────────────────────┐
│                  Transcription Server                            │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │  [Flask Receiver]                                          │ │
│  │       ↓                                                     │ │
│  │  [faster-whisper / OpenAI Whisper API]  ← ASR             │ │
│  │       ↓ (raw transcript text)                              │ │
│  │  [Claude API]  ← Cleanup, summarize, extract action items  │ │
│  │       ↓                                                     │ │
│  │  [Output] (markdown file / email / storage)                │ │
│  └────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

Key design decisions:

- **Separate ASR from Claude:** Claude API doesn't accept raw audio, so a speech-to-text engine (Whisper) must run first to produce transcript text, which Claude then refines.
- **WiFi only on-demand:** WiFi radio stays off during recording to preserve battery and prevent audio glitches on the single-core ESP32.
- **Local-first pipeline:** Whisper can run locally on your infrastructure (`faster-whisper`) or via a hosted API—you choose.

## Hardware Requirements

| Component | Notes |
|---|---|
| **ESP32-C6-LCD-1.69** | All-in-one: MCU, display, microphone, codec, speaker amp, battery charging. Get it from Waveshare. |
| **microSD card breakout** | Common SPI-based module (~$2–5), wired to free GPIO pins. |
| **microSD card** | 32GB recommended. |
| **3.7V Li-Po battery** | Plugs into the board's MX1.25 connector. 5000 mAh gives ~8 hours of recording. |
| **Speaker** | Wired to the board's 2-pin speaker connector (optional; audio output from codec). |
| **USB-C cable** | For charging and firmware uploads. |

See [`build-guide.md` section 1](build-guide.md#1-hardware-overview) for detailed hardware breakdown, and [`build-guide.md` section 2](build-guide.md#2-wiring-sd-card-module) for SD card wiring.

## File Structure Explained

### `firmware/`

A PlatformIO project (Arduino-based C++). Handles:
- Button input (gesture recognition: short-press vs. long-press)
- I2S audio capture from the ES8311 codec
- WAV file writing to SD card
- Waveform visualization on the LCD
- Menu state machine (idle, recording, upload, info screens)
- WiFi upload of .wav files to the server

**Before building:** see [`firmware/README.md`](firmware/README.md) for configuration requirements.

### `server/`

A Python Flask server (with optional Docker deployment). Handles:
- Receive `.wav` file uploads from the device
- Transcribe using `faster-whisper` (local) or OpenAI Whisper API
- Send transcript text to Claude for cleanup, summarization, action items
- Write results to markdown file and/or email
- Support for batch uploads from the device

**Deployment options:**
- Local: `python receive_and_transcribe.py` or `docker-compose up`
- Remote Docker host via SSH: `./deploy.sh deploy` (configured for `savage.local`)

See [`server/README.md`](server/README.md) for detailed setup.

## Documentation

- **[`build-guide.md`](build-guide.md)** — Hardware overview, wiring instructions, system architecture, firmware behavior, transcription pipeline, and suggested build order. **Start here if you're assembling the hardware.**
- **[`firmware/README.md`](firmware/README.md)** — Firmware-specific configuration, build commands, and smoke testing.
- **[`server/README.md`](server/README.md)** — Server deployment, environment configuration, and usage.

## Configuration

### Firmware (`firmware/src/`)

- **`pins.h`** — GPIO assignments (TODO: fill in from Waveshare schematic)
- **`config.h`** — WiFi SSID/password, upload server URL, timeouts
- **`es8311_codec.h`** — Audio codec register sequence (adjust if audio is distorted)

### Server (`server/`)

- **`.env`** — Copy from `.env.example`, set `ANTHROPIC_API_KEY`, upload path, email settings, etc.
- **`docker-compose.yml`** — Port mapping, volume mounts, environment variables
- **`deploy.sh`** — SSH context setup and remote Docker deployment (if using `savage.local` or similar)

## Transcription Pipeline Caveat

⚠️ **Important:** Claude's API does not accept raw audio. The pipeline always requires a separate speech-to-text step:

1. **Whisper (local):** Run `faster-whisper` or `whisper.cpp` on your infrastructure (free, private, slower)
2. **Whisper (hosted):** Use OpenAI Whisper API, Deepgram, or AssemblyAI (costs ~$0.02–0.10 per meeting, faster)
3. **Claude:** Feed the transcript text to Claude for summarization and action items

The delivered `server/receive_and_transcribe.py` defaults to local `faster-whisper`. Swapping to a hosted ASR API is a one-line change—see the server README.

## Power & Battery Life

- **Recording:** ~5–8 hours on a 5000 mAh 3.7V Li-Po (depending on audio levels and codec efficiency)
- **Idle screen (WiFi off):** weeks
- **Battery charging:** via USB-C, built-in charge IC handles the rest

The ESP32-C6 and codec draw ~50 mA during recording. The LCD draws ~20 mA. WiFi radio is off during recording (major battery saver).

## Extending the Project

### Add a Second Button

The current firmware uses one button with short/long-press gestures (cycle through menu, select). Wiring a second button to a free GPIO would let you implement dedicated "next" and "select" buttons for faster menu navigation. See `firmware/src/main.cpp` for the button logic.

### Use a Hosted ASR API

Swap `faster-whisper` for OpenAI Whisper, Deepgram, or AssemblyAI by editing the `transcribe()` function in `server/receive_and_transcribe.py`. Each API has its own library and cost model.

### Add Shake-to-Start

The ESP32-C6 has a built-in 6-axis IMU (QMI8658). You can add orientation detection or shake-to-start by reading the IMU registers and triggering recording via motion. See Waveshare's demo code for IMU examples.

### Email Transcripts

The receiver script already supports sending emails via SMTP. Set `SMTP_SERVER`, `SMTP_PORT`, `SMTP_FROM`, `SMTP_PASSWORD`, and `RECIPIENT_EMAIL` in `.env`, then enable email output in the Python script.

## License

[Specify your license here, e.g., MIT, Apache 2.0, GPL-3.0, etc.]

## Support & Troubleshooting

- **Firmware won't build?** Check that PlatformIO is installed and the project is opened in VS Code with the PlatformIO sidebar.
- **SD card not detected?** Verify GPIO pins in `pins.h` against Waveshare's schematic; confirm wiring per `build-guide.md` section 2.
- **Microphone silent?** Review the codec init sequence in `es8311_codec.h` and the I2S capture setup in `i2s_capture.h`.
- **WiFi upload fails?** Ensure the server is reachable and `UPLOAD_SERVER_URL` in `config.h` is correct.
- **Transcription errors?** Check server logs for ASR/Claude API failures; ensure `ANTHROPIC_API_KEY` is set.

See [`build-guide.md`](build-guide.md) for more details on architecture and design decisions, and the component-specific READMEs for focused troubleshooting.

## Contributing

Contributions are welcome! Please open an issue or pull request with your improvements.

---

**GitHub:** [github.com/merlinmb/esp32_ai-meeting-buddy](https://github.com/merlinmb/esp32_ai-meeting-buddy)

Last updated: 2026-08-28
