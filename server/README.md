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
| `GET /api/apple-shortcut/data` | Read-only payload for the generated Apple Shortcut; includes note content and open tasks and accepts `TODOS_READ_TOKEN` |
| `GET /api/recap?period=day|week|month` | LLM recap of recent notes |
| `POST /api/todos/command` `{"text"}` | Voice-style "mark the dentist task done" |
| `GET/POST /api/meetings/<id>/moments` | Marked moments (ms offsets) sent by the device |
| `GET /api/meetings/<id>/memory` | Stored structured memory |
| `POST /api/brain/backfill` | Queue memory extraction for notes that predate this feature |

The dashboard has an **Ask** button (ask, today/week briefing, recap) and shows due dates in the to-do list. The `meeting-buddy-mcp` service gains `ask_notes`, `get_agenda`, `get_calendar` and `get_recap` tools. No new containers are required; semantic search just needs an embedding model pulled in your existing Ollama.

## Import notes and tasks into Apple Notes and Reminders

The dashboard can generate a personalized iPhone Shortcut that imports completed, unarchived notes into Apple Notes and open tasks into Reminders. It uses `https://meetings.mgbeets.com` and embeds `TODOS_READ_TOKEN` in the Shortcut's request header.

1. Set a long random `TODOS_READ_TOKEN` in the server's `.env` and redeploy. This token can read the server's read-only report/agenda endpoints and the dedicated Apple export payload (including note content), but cannot upload recordings or change notes or tasks. Keep it private.
2. On the iPhone, open Safari and sign in to `https://meetings.mgbeets.com` with the dashboard account.
3. From the dashboard, tap **iPhone Shortcut**, or navigate to `https://meetings.mgbeets.com/apple-shortcut/download`. The authenticated endpoint generates the `.shortcut` file with the configured read token embedded; if the read token is unset, the server explains how to enable it.
4. Open the downloaded file and follow iOS's Shortcuts import prompts. Review its actions and allow access to Notes and Reminders when asked. If iOS blocks importing an unsigned shortcut on your version, this download cannot bypass Apple's trust checks; use Apple's supported sharing/import flow or create and trust the shortcut in the Shortcuts app.
5. Run **AI Meeting Buddy Sync** once to test it. For automatic imports, create a personal automation in Shortcuts and select this shortcut.

The generated Shortcut uses stable Meeting Buddy IDs to avoid creating duplicates on later runs. It creates reminders with due dates when the server has a due time. This is a **one-way import**: edits, deletions, and completion in Apple apps do not sync back, and server-side edits do not update already imported Apple items. The embedded token is readable by anyone who can access the Shortcut on the iPhone; revoke it by changing `TODOS_READ_TOKEN` and downloading the Shortcut again.

The following manual setup is an alternative if you prefer to build the Shortcut yourself.

### 1. Make the server reachable from your iPhone

For a home deployment, connect the iPhone to the same Wi-Fi as `savage.local` and use `http://savage.local:8787` as the server address. If that name does not resolve on the iPhone, use the server's LAN IP address instead.

For access away from home, use a VPN into your home network or put the server behind an HTTPS reverse proxy. Do not expose the server over plain HTTP to the public internet: the Shortcut sends its API token with every request.

### 2. Prepare API tokens

The notes endpoint requires `UPLOAD_TOKEN`. The tasks endpoint accepts either `UPLOAD_TOKEN` or the narrower, read-only `TODOS_READ_TOKEN`. For better separation, set a long random `TODOS_READ_TOKEN` in the server's `.env`, then redeploy the service so it takes effect. Keep both tokens private; do not share the Shortcut if it contains them.

Use the `UPLOAD_TOKEN` for the notes request, and the `TODOS_READ_TOKEN` for the tasks request. If you have not configured `TODOS_READ_TOKEN`, use `UPLOAD_TOKEN` for both requests instead.

### 3. Create the Shortcut

On the iPhone, open **Shortcuts**, tap **+**, and create a shortcut such as **Import Meeting Buddy**. Add these actions in order:

1. Add **URL** and enter `https://your-server.example/api/notes?include=content` (replace the host with the address from step 1; use `http://savage.local:8787` for a trusted home-network-only setup).
2. Add **Get Contents of URL**. Set the method to **GET**. Under **Headers**, add `X-Upload-Token` with the server's `UPLOAD_TOKEN` value.
3. Turn the response into a list of notes:
   - Search actions for **Get Dictionary from Input** and add it after **Get Contents of URL**. Its input should be the output of **Get Contents of URL** (shown as a blue variable/magic-variable token). If Shortcuts did not fill this in automatically, tap the input and select **Contents of URL**.
   - Add **Get Dictionary Value** after it. Set the key to `notes` and the dictionary to the output of **Get Dictionary from Input**. This action's output is the notes list.
   - Add **Repeat with Each** after that and use the output of **Get Dictionary Value** as its input. Everything added between **Repeat with Each** and **End Repeat** runs once per note; **Repeat Item** is the current note dictionary.
   - Important: `id`, `name`, and `content` are **dictionary keys**, not variables shown in the picker. To retrieve one, add **Get Dictionary Value**, set its key by typing `id` literally, and set its dictionary/input to the blue **Repeat Item** variable. The action's result is that note's ID.
   - To make that result easy to reuse, add **Set Variable** immediately after the lookup and name it `Note ID`. Repeat the same two actions for the other fields: **Get Dictionary Value** with key `name` from **Repeat Item**, then **Set Variable** named `Note Name`; and key `content` from **Repeat Item**, then **Set Variable** named `Note Content`. Type the keys into the action; do not look for them in the variable picker.
   - To choose **Repeat Item** for the dictionary input, tap that input field, tap **Select Variable** (or the variable/magic-variable button above the keyboard), then choose **Repeat Item**. To save a lookup result, add **Set Variable** immediately after the lookup, tap the variable-name field in that action, and type the name above. Its input should be the lookup's result; if needed, tap the input and choose the preceding **Get Dictionary Value** action's output in the variable picker.
4. Add the following actions **inside** the notes' **Repeat with Each** block, after the three **Set Variable** actions:
   - Add **Text**. Type `[Meeting Buddy #`, insert the `Note ID` variable using the variable picker, then type `]`. To insert it, tap the Text action where the variable should go, open **Select Variable**, and choose **Note ID**. For note ID 42, this produces `[Meeting Buddy #42]`.
   - Add **Find Notes** and filter **Name** **contains** the marker. Tap the filter's value and insert the output of the marker **Text** action from the variable picker.
   - Add **Count** to count the results from **Find Notes**, then add **If** and set the condition to **Count** **is** `0`. This means no note with the marker was found. In the **If** branch, add a **Text** action: type `Meeting Buddy - `, insert `Note Name`, type a space, insert the marker output, press Return twice, then insert `Note Content`. Use the variable picker for each insertion; do not type variable names or angle-bracket placeholders as text. Add **Create Note** under this Text action, use the Text output as its body, and choose the Notes folder. Leave the **Otherwise** branch empty; an existing matching note is skipped.
   - Keep the **Find Notes**, **Count**, **If**, and **Create Note** actions before the notes' **End Repeat**, so they run once for each server note.
5. After the notes repeat, add another **URL** action for `https://your-server.example/api/todos?done=0`, followed by **Get Contents of URL** configured as **GET**. Set `X-Upload-Token` to the `TODOS_READ_TOKEN` (or `UPLOAD_TOKEN` if you did not configure the read-only token).
6. Parse the tasks response the same way as the notes response: add **Get Dictionary from Input** after the second **Get Contents of URL**, then **Get Dictionary Value** with key `todos`, then **Repeat with Each** using that action's output. Inside the task repeat, retrieve fields by adding **Get Dictionary Value**, setting its dictionary to **Repeat Item** and typing the key (`id`, `text`, `meeting_name`, or `due_utc`) into the key field. These are literal keys, not variables to select. Follow each lookup with **Set Variable** (for example, `Task ID`, `Task Text`, `Meeting Name`, `Due UTC`) if you need to reuse its result. Build the reminder title with a **Text** action by inserting `Task Text`, typing ` [Meeting Buddy #`, inserting `Task ID`, and typing `]`. Use **Find Reminders** to look for a reminder whose title contains this marker, then **Count** and create a reminder only when the count is `0`. When `due_utc` is not empty, pass the `Due UTC` variable through **Get Dates from Input** and use the resulting date for the reminder's due date; otherwise leave the due date unset. The API time is UTC, and iOS displays it in the phone's local time zone. You can include `Meeting Name` in the reminder's notes for context.
7. Run the Shortcut once and approve its access to the server, Notes, and Reminders when iOS prompts you. Check the results in the selected Notes folder and in Reminders.

The note API returns completed, unarchived notes, including their processed Markdown bodies. The task API returns open tasks only; tasks already marked done in Meeting Buddy are excluded. To import completed tasks too, remove `?done=0`, but they will be created as active Reminders unless you add a separate completion-state check.

### 4. Run it automatically (optional)

In **Shortcuts → Automation**, create a **Time of Day** personal automation, choose a schedule, and select **Run Shortcut → Import Meeting Buddy**. Keep the marker-based **Find** checks in the Shortcut so each scheduled run creates only newly seen items. Existing Apple items are not refreshed from the server, and completing a Reminder does not mark the Meeting Buddy task done.

## Device (PalaNote) additions

* **Moments:** a short press of the power button during a recording marks that point; the offsets are uploaded with the note. Long-recording mode (`recordToggle`, up to `MAX_RECORDING_MINUTES`) is unchanged.
* **Multiple Wi-Fi networks / volume** over USB serial (115200): `WIFI+ ssid|password`, `WIFI-ssid`, `WIFI?`, `VOL 10..100`, `STATUS`. With no stored profiles the device uses `secrets.h` exactly as before; it is always kept as the fallback.
* **Index recovery:** `index.tmp` is used if power is lost mid-save.
* Sleep, wake and battery code are untouched.
