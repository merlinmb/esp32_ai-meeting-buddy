# AI Meeting Buddy - companion receiver

Receives uploaded recordings, transcribes them, and asks Claude to clean the transcript up into a summary + action items. Two ways to run it: directly with Python, or as a Docker container deployed to `savage.local`.

## Option A: Docker on savage.local (recommended)

This is the "set it up once, forget about it" path - the container runs on `savage.local` so your laptop doesn't need to be on and awake for uploads to work.

```bash
cp .env.example .env      # fill in ANTHROPIC_API_KEY at minimum
./deploy.sh setup         # one-time: registers a Docker context pointing at savage.local over SSH
./deploy.sh deploy        # builds the image ON savage.local and starts it
```

That's it - `deploy.sh` streams this folder to savage.local's Docker daemon over SSH and builds/runs it there, so there's nothing to manually copy over. See the comments at the top of `deploy.sh` for the SSH/Docker prerequisites on savage.local, and `./deploy.sh {logs|down|restart|pull-transcripts}` for day-to-day use.

Transcripts are stored in a Docker named volume on savage.local (not a plain folder - see the comment in `docker-compose.yml` for why). Run `./deploy.sh pull-transcripts` any time to copy them down to `./transcripts_from_savage` on your machine.

The firmware's `config.h` already points `UPLOAD_SERVER_URL` at `http://savage.local:8787/upload` to match this deployment.

## Option B: plain Python (for local testing without Docker)

```bash
python3 -m venv venv
source venv/bin/activate         # Windows: venv\Scripts\activate
pip install -r requirements.txt
cp .env.example .env
python receive_and_transcribe.py
```

If you use this instead of the Docker deployment, update the firmware's `UPLOAD_SERVER_URL` to point at whichever machine runs this (e.g. `http://192.168.1.50:8787/upload`).

## Configuration (`.env`)

- `ANTHROPIC_API_KEY` - required. Get one at https://console.anthropic.com/
- `WHISPER_MODEL` - `small` is a good default; use `base` if transcription is too slow, `medium`/`large-v3` for better accuracy if the machine (savage.local) has the CPU/RAM or a GPU for it.
- Email settings only if you want transcripts emailed rather than (or in addition to) saved.

## Swapping in a hosted transcription API

If you'd rather not run Whisper at all, edit `transcribe_audio()` in `receive_and_transcribe.py` to call a hosted ASR API instead (OpenAI, Deepgram, AssemblyAI, etc.) - it just needs to return a text string. Nothing else in the file, nor the Docker setup, needs to change.
