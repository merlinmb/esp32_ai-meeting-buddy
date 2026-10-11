# AI Meeting Buddy - companion receiver

Receives uploaded recordings, transcribes them, and asks Claude to clean the transcript up into a summary + action items. Two ways to run it: directly with Python, or as a Docker container deployed to `savage.local`.

Every upload is tracked (received time, status, errors) in a small SQLite database and browsable at `http://<host>:8787/` - a dashboard listing all recordings with their state (received / transcribing / summarizing / completed / failed), a "View" button for the finished transcript, and a "Resubmit to Claude" button to retry the cleanup step (e.g. after an API error) without re-transcribing the audio.

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
- `ADMIN_USERNAME` / `ADMIN_PASSWORD` - required. Login for the dashboard (the web UI). Pick a real password before deploying anywhere reachable off your own machine.
- `UPLOAD_TOKEN` - required. Shared secret the ESP32 device must send on every request to `/upload/status` and `/upload/chunk` (the resumable, chunked upload path it actually uses - see the comment above those routes in `receive_and_transcribe.py`) as well as the plain `/upload` the browser dashboard's recorder uses; separate from the dashboard login since the device can't do a browser login flow. Generate with `python -c "import secrets; print(secrets.token_urlsafe(32))"` and put the same value in `firmware/src/config.h`'s `UPLOAD_TOKEN`.
- `FLASK_SECRET_KEY` - required. Signs session cookies. Generate the same way as `UPLOAD_TOKEN`.
- `BEHIND_HTTPS_PROXY` - set to `true` once this is deployed behind a reverse proxy that terminates HTTPS (nginx, Caddy, Cloudflare Tunnel, etc.). Makes session cookies HTTPS-only, enables HSTS, and switches the app to a production WSGI server (waitress) instead of Flask's dev server. Leave `false` for local `http://` testing.
- `WHISPER_MODEL` - `small` is a good default; use `base` if transcription is too slow, `medium`/`large-v3` for better accuracy if the machine (savage.local) has the CPU/RAM or a GPU for it.
- Email settings only if you want transcripts emailed rather than (or in addition to) saved.

## Exposing this publicly

This app is only as secure as the deployment around it:

- Put a reverse proxy in front that terminates HTTPS (Caddy is the easiest - automatic Let's Encrypt certs with a two-line config; nginx or Cloudflare Tunnel also work). Set `BEHIND_HTTPS_PROXY=true` once that's in place.
- Set a real `ADMIN_PASSWORD` - not the placeholder in `.env.example`.
- Keep `UPLOAD_TOKEN` and `FLASK_SECRET_KEY` secret; anyone with `UPLOAD_TOKEN` can submit audio for transcription (burning your Claude API budget), and anyone with `FLASK_SECRET_KEY` can forge login sessions.
- `.env` itself must never be committed - it already is gitignored, double check before pushing.

## Swapping in a hosted transcription API

If you'd rather not run Whisper at all, edit `transcribe_audio()` in `receive_and_transcribe.py` to call a hosted ASR API instead (OpenAI, Deepgram, AssemblyAI, etc.) - it just needs to return a text string. Nothing else in the file, nor the Docker setup, needs to change.

## Second-brain features

After the normal cleanup, each completed note also gets a structured *memory* (title, summary, people, decisions, facts, importance) and any tasks/appointments in it, with dates resolved server-side against LOCAL_TIMEZONE (the LLM only reports what was said; Python computes the absolute time). If extraction fails the note still completes and the original to-do extraction is used. See the second-brain block in .env.example.

| Endpoint | Purpose |
|---|---|
| `POST /api/ask` `{"question"}` | Answer from your notes (keyword search; plus embeddings if `EMBEDDING_MODEL` is set) |
| `GET /api/agenda`, `/api/briefing/daily`, `/api/briefing/weekly` | Appointments, due and overdue tasks, with speakable `text` |
| `GET /api/calendar?from=&days=` | Dated open tasks/events |
| `GET /api/apple-shortcut/pending` | Notes and open tasks not yet sent to the Apple Shortcut (marks them sent; `?peek=1` doesn't, `?reset=1` resends everything); accepts `TODOS_READ_TOKEN` |
| `GET /api/recap?period=day|week|month` | LLM recap of recent notes |
| `POST /api/todos/command` `{"text"}` | Voice-style "mark the dentist task done" |
| `GET/POST /api/meetings/<id>/moments` | Marked moments (ms offsets) sent by the device |
| `GET /api/meetings/<id>/memory` | Stored structured memory |
| `POST /api/brain/backfill` | Queue memory extraction for notes that predate this feature |

The dashboard has an **Ask** button (ask, today/week briefing, recap) and shows due dates in the to-do list. The `meeting-buddy-mcp` service gains `ask_notes`, `get_agenda`, `get_calendar` and `get_recap` tools. No new containers are required; semantic search just needs an embedding model pulled in your existing Ollama.

## Import notes and tasks into Apple Notes and Reminders

iOS only imports *signed* shortcuts, and signing needs macOS, so the server can't hand you a ready-made `.shortcut`. Instead the server tracks what it has already sent, which keeps the shortcut you build on the iPhone down to a few actions.

1. Set a long random `TODOS_READ_TOKEN` in `.env` and redeploy. It can read the sync endpoint (which includes note content) but cannot upload recordings or change notes or tasks.
2. Sign in to the dashboard on the iPhone and tap **iPhone Shortcut**. The page shows your URL and token and the steps for building the shortcut in the Shortcuts app.
3. Run the shortcut, or schedule it with a **Time of Day** personal automation.

`GET /api/apple-shortcut/pending` returns `{notes, todos, dated_todos}`: completed, unarchived notes and open tasks that have not been sent before (`dated_todos` are the tasks with a `due_utc`). Everything returned is marked as sent, so the next call returns only new items; add `?peek=1` to look without marking. If a run fails after the fetch (for example, a permission prompt was declined) those items are not re-sent; open the endpoint once with `?reset=1` to forget everything sent so far and get it all again. Items already imported will be created a second time, so delete them from Notes/Reminders first.

This is a **one-way import**: edits, deletions, and completion in Apple apps do not sync back, and later server-side edits do not update items already imported. The token is readable by anyone who can open the shortcut; revoke it by changing `TODOS_READ_TOKEN`.

## Device (PalaNote) additions

* **Moments:** a short press of the power button during a recording marks that point; the offsets are uploaded with the note. Long-recording mode (`recordToggle`, up to `MAX_RECORDING_MINUTES`) is unchanged.
* **Multiple Wi-Fi networks / volume** over USB serial (115200): `WIFI+ ssid|password`, `WIFI-ssid`, `WIFI?`, `VOL 10..100`, `STATUS`. With no stored profiles the device uses `secrets.h` exactly as before; it is always kept as the fallback.
* **Index recovery:** `index.tmp` is used if power is lost mid-save.
* Sleep, wake and battery code are untouched.
