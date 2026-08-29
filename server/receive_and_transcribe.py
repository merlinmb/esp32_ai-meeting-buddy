#!/usr/bin/env python3
"""
AI Meeting Buddy - companion receiver.

Receives a WAV file uploaded by the ESP32-C6 device, transcribes it locally
with faster-whisper (no audio leaves this machine), sends the raw transcript
to Claude for cleanup/summary/action items, then saves the result as a
markdown file and optionally emails it.

Why the two-step pipeline: Claude's API is text-in/text-out (plus images and
PDFs) - it does not accept raw audio for transcription. So something has to
turn speech into text first; that's faster-whisper here. Claude's job is the
second step: turning a rough transcript into something actually useful.

Every upload is tracked in a small SQLite database (status, timestamps,
errors) and browsable through a web UI at "/", which also lets you resubmit
a meeting's raw transcript to Claude without needing to re-transcribe.

Run:
    pip install -r requirements.txt
    cp .env.example .env   # then fill in ANTHROPIC_API_KEY at minimum
    python receive_and_transcribe.py
"""

import functools
import hmac
import os
import queue
import secrets
import smtplib
import threading
import time
import traceback
import wave
from datetime import datetime
from email.mime.text import MIMEText
from pathlib import Path

from dotenv import load_dotenv
from flask import Flask, request, jsonify, render_template, send_file, abort, session, redirect, url_for
from werkzeug.middleware.proxy_fix import ProxyFix
from werkzeug.security import generate_password_hash, check_password_hash

from db import MeetingStore, STATUS_TRANSCRIBING, STATUS_SUMMARIZING, STATUS_FAILED

load_dotenv(override=True)  # .env should win over any stray OS-level env vars of the same name

ANTHROPIC_API_KEY = os.environ.get("ANTHROPIC_API_KEY", "")
LLM_PROVIDER = os.environ.get("LLM_PROVIDER", "claude").strip().lower()
OLLAMA_URL = os.environ.get("OLLAMA_URL", "http://savage.local:11434")
OLLAMA_MODEL = os.environ.get("OLLAMA_MODEL", "llama3.1")
UPLOAD_PORT = int(os.environ.get("UPLOAD_PORT", "8787"))
OUTPUT_DIR = Path(os.environ.get("OUTPUT_DIR", "./transcripts"))
WHISPER_MODEL = os.environ.get("WHISPER_MODEL", "small")
WHISPER_DEVICE = os.environ.get("WHISPER_DEVICE", "cpu")
WHISPER_COMPUTE_TYPE = os.environ.get("WHISPER_COMPUTE_TYPE") or ("int8" if WHISPER_DEVICE == "cpu" else "float16")
BEHIND_HTTPS_PROXY = os.environ.get("BEHIND_HTTPS_PROXY", "false").lower() == "true"

ADMIN_USERNAME = os.environ.get("ADMIN_USERNAME", "")
ADMIN_PASSWORD = os.environ.get("ADMIN_PASSWORD", "")
UPLOAD_TOKEN = os.environ.get("UPLOAD_TOKEN", "")
FLASK_SECRET_KEY = os.environ.get("FLASK_SECRET_KEY", "")

if not ADMIN_USERNAME or not ADMIN_PASSWORD:
    raise RuntimeError(
        "ADMIN_USERNAME and ADMIN_PASSWORD must be set in .env to protect the dashboard - "
        "see .env.example."
    )
if not UPLOAD_TOKEN:
    raise RuntimeError(
        "UPLOAD_TOKEN must be set in .env to protect /upload - see .env.example. "
        "Generate one with: python -c \"import secrets; print(secrets.token_urlsafe(32))\""
    )
if not FLASK_SECRET_KEY:
    raise RuntimeError(
        "FLASK_SECRET_KEY must be set in .env to sign session cookies - see .env.example."
    )

ADMIN_PASSWORD_HASH = generate_password_hash(ADMIN_PASSWORD)

EMAIL_ENABLED = os.environ.get("EMAIL_ENABLED", "false").lower() == "true"
SMTP_HOST = os.environ.get("SMTP_HOST", "")
SMTP_PORT = int(os.environ.get("SMTP_PORT", "587"))
SMTP_USERNAME = os.environ.get("SMTP_USERNAME", "")
SMTP_PASSWORD = os.environ.get("SMTP_PASSWORD", "")
EMAIL_FROM = os.environ.get("EMAIL_FROM", "")
EMAIL_TO = os.environ.get("EMAIL_TO", "")

OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
RAW_AUDIO_DIR = OUTPUT_DIR / "raw_audio"
RAW_AUDIO_DIR.mkdir(exist_ok=True)

app = Flask(__name__)
app.secret_key = FLASK_SECRET_KEY
app.config.update(
    SESSION_COOKIE_HTTPONLY=True,
    SESSION_COOKIE_SAMESITE="Lax",
    SESSION_COOKIE_SECURE=BEHIND_HTTPS_PROXY,
    PERMANENT_SESSION_LIFETIME=60 * 60 * 12,  # 12 hours
)
if BEHIND_HTTPS_PROXY:
    # Trust one hop of X-Forwarded-Proto/Host from the reverse proxy that
    # terminates TLS in front of this app, so request.is_secure and
    # generated URLs reflect https:// instead of the plain http:// this
    # process actually receives from the proxy.
    app.wsgi_app = ProxyFix(app.wsgi_app, x_for=1, x_proto=1, x_host=1)


@app.after_request
def add_security_headers(response):
    response.headers["X-Content-Type-Options"] = "nosniff"
    response.headers["X-Frame-Options"] = "DENY"
    response.headers["Referrer-Policy"] = "same-origin"
    if BEHIND_HTTPS_PROXY:
        response.headers["Strict-Transport-Security"] = "max-age=31536000; includeSubDomains"
    return response


@app.errorhandler(401)
@app.errorhandler(404)
def api_json_error(e):
    # Flask's default abort() error pages are HTML, which breaks any
    # fetch(...).json() call in the dashboard with a confusing "Unexpected
    # token '<'" / "did not match the expected pattern" - most commonly on
    # session expiry (401). Return JSON for the fetch-based routes so the
    # frontend can show a clear message instead.
    if request.path.startswith("/api/") or request.path in ("/upload", "/upload/status", "/upload/chunk"):
        return jsonify({"error": e.description or e.name}), e.code
    return e


store = MeetingStore(OUTPUT_DIR / "meetings.db")

_whisper_model = None  # lazy-loaded so the server starts fast

# ---------------------------------------------------------------------------
# Auth: session login for the dashboard, separate shared-secret token for the
# ESP32 device's unattended /upload POSTs (it can't do a browser login flow).
# ---------------------------------------------------------------------------

_login_attempts = {}  # ip -> (failure_count, window_start_monotonic)
LOGIN_RATE_LIMIT = 5
LOGIN_RATE_WINDOW_SECONDS = 300


def _client_ip() -> str:
    return request.remote_addr or "unknown"


def _login_rate_limited(ip: str) -> bool:
    count, window_start = _login_attempts.get(ip, (0, 0.0))
    if time.monotonic() - window_start > LOGIN_RATE_WINDOW_SECONDS:
        return False
    return count >= LOGIN_RATE_LIMIT


def _record_login_failure(ip: str):
    count, window_start = _login_attempts.get(ip, (0, 0.0))
    now = time.monotonic()
    if now - window_start > LOGIN_RATE_WINDOW_SECONDS:
        count, window_start = 0, now
    _login_attempts[ip] = (count + 1, window_start)


def _record_login_success(ip: str):
    _login_attempts.pop(ip, None)


def login_required(view):
    @functools.wraps(view)
    def wrapped(*args, **kwargs):
        if not session.get("logged_in"):
            if request.path.startswith("/api/"):
                abort(401)
            return redirect(url_for("login", next=request.path))
        return view(*args, **kwargs)
    return wrapped


def upload_token_required(view):
    """Allows the ESP32 device (shared X-Upload-Token header, no browser
    login flow available) or the dashboard's own browser-based recorder
    (already authenticated via session cookie) to POST to /upload."""
    @functools.wraps(view)
    def wrapped(*args, **kwargs):
        if session.get("logged_in"):
            return view(*args, **kwargs)
        supplied = request.headers.get("X-Upload-Token", "")
        if not supplied or not hmac.compare_digest(supplied, UPLOAD_TOKEN):
            abort(401)
        return view(*args, **kwargs)
    return wrapped


@app.route("/login", methods=["GET", "POST"])
def login():
    error = None
    if request.method == "POST":
        ip = _client_ip()
        if _login_rate_limited(ip):
            print(f"Login rate-limited for {ip}")
            error = "Too many failed attempts - try again in a few minutes."
        else:
            username = request.form.get("username", "")
            password = request.form.get("password", "")
            valid = hmac.compare_digest(username, ADMIN_USERNAME) and check_password_hash(
                ADMIN_PASSWORD_HASH, password
            )
            if valid:
                print(f"Login succeeded for {ip}")
                _record_login_success(ip)
                session.clear()
                session["logged_in"] = True
                session.permanent = True
                next_path = request.args.get("next") or url_for("dashboard")
                if not next_path.startswith("/"):
                    next_path = url_for("dashboard")
                return redirect(next_path)
            print(f"Login failed for {ip} (username={username!r})")
            _record_login_failure(ip)
            error = "Invalid username or password."
    return render_template("login.html", error=error)


@app.route("/logout", methods=["POST"])
def logout():
    session.clear()
    return redirect(url_for("login"))


def get_whisper_model():
    global _whisper_model
    if _whisper_model is None:
        from faster_whisper import WhisperModel
        print(f"Loading faster-whisper model '{WHISPER_MODEL}' on {WHISPER_DEVICE} "
              f"({WHISPER_COMPUTE_TYPE}) (first run downloads it)...")
        _whisper_model = WhisperModel(WHISPER_MODEL, device=WHISPER_DEVICE, compute_type=WHISPER_COMPUTE_TYPE)
    return _whisper_model


def transcribe_audio(wav_path: Path) -> str:
    """Local speech-to-text pass. Swap this function's body for a hosted ASR
    API (OpenAI, Deepgram, AssemblyAI, ...) if you'd rather not run Whisper
    locally - nothing else in this file needs to change."""
    model = get_whisper_model()
    start = time.monotonic()
    segments, info = model.transcribe(str(wav_path), beam_size=5)
    text = " ".join(seg.text.strip() for seg in segments)
    elapsed = time.monotonic() - start
    print(f"Transcribed {wav_path.name}: {info.duration:.1f}s audio -> "
          f"{len(text)} chars in {elapsed:.1f}s ({info.duration / elapsed:.1f}x realtime)")
    return text


def build_cleanup_prompt(raw_transcript: str, meeting_name: str) -> str:
    return f"""You are cleaning up a raw speech-to-text transcript of a meeting called "{meeting_name}".
The transcript below may contain filler words, run-on sentences, and transcription
errors from an automatic speech recognizer - it has no speaker labels.

Produce a markdown document with these sections, in this order:
1. "## Summary" - a short paragraph on what the meeting was about.
2. "## Key points" - a bulleted list of the substantive points discussed.
3. "## Action items" - a bulleted list of anything that sounds like a task,
   decision, or follow-up someone should act on. If there are none, say so.
4. "## Cleaned transcript" - the transcript rewritten into readable prose
   with filler words and obvious ASR artifacts removed, but without changing
   what was actually said or inventing content that isn't there.

Raw transcript:
---
{raw_transcript}
---
"""


def clean_up_with_claude(prompt: str) -> str:
    if not ANTHROPIC_API_KEY:
        raise RuntimeError("ANTHROPIC_API_KEY is not set in .env")

    import anthropic
    client = anthropic.Anthropic(api_key=ANTHROPIC_API_KEY)

    response = client.messages.create(
        model="claude-sonnet-5",
        max_tokens=4096,
        messages=[{"role": "user", "content": prompt}],
    )
    print(f"Claude cleanup: {response.usage.input_tokens} in / {response.usage.output_tokens} out tokens")
    return next(block.text for block in response.content if block.type == "text")


def clean_up_with_ollama(prompt: str) -> str:
    import requests
    response = requests.post(
        f"{OLLAMA_URL}/api/generate",
        json={"model": OLLAMA_MODEL, "prompt": prompt, "stream": False},
        timeout=300,
    )
    response.raise_for_status()
    body = response.json()
    print(f"Ollama cleanup ({OLLAMA_MODEL}): {body.get('eval_count', '?')} tokens "
          f"in {body.get('eval_duration', 0) / 1e9:.1f}s")
    return body["response"]


def clean_up_transcript(raw_transcript: str, meeting_name: str) -> str:
    """Turns the rough ASR transcript into a cleaned-up markdown summary,
    via either Claude or a local Ollama model depending on LLM_PROVIDER."""
    print(f"Cleaning up '{meeting_name}' with {LLM_PROVIDER} ({len(raw_transcript)} chars of raw transcript)...")
    prompt = build_cleanup_prompt(raw_transcript, meeting_name)
    start = time.monotonic()
    result = clean_up_with_ollama(prompt) if LLM_PROVIDER == "ollama" else clean_up_with_claude(prompt)
    print(f"Cleanup for '{meeting_name}' finished in {time.monotonic() - start:.1f}s")
    return result


def send_email(subject: str, body_markdown: str):
    if not EMAIL_ENABLED:
        return
    if not (SMTP_HOST and SMTP_USERNAME and SMTP_PASSWORD and EMAIL_FROM and EMAIL_TO):
        print("EMAIL_ENABLED is true but SMTP settings are incomplete - skipping email.")
        return
    send_email_to(EMAIL_TO, subject, body_markdown)


def send_email_to(to_address: str, subject: str, body_markdown: str):
    if not (SMTP_HOST and SMTP_USERNAME and SMTP_PASSWORD and EMAIL_FROM):
        raise RuntimeError("SMTP settings are incomplete in .env - cannot send email")

    msg = MIMEText(body_markdown, "plain", "utf-8")
    msg["Subject"] = subject
    msg["From"] = EMAIL_FROM
    msg["To"] = to_address

    with smtplib.SMTP(SMTP_HOST, SMTP_PORT) as server:
        server.starttls()
        server.login(SMTP_USERNAME, SMTP_PASSWORD)
        server.sendmail(EMAIL_FROM, [to_address], msg.as_string())


def run_summarize_and_save(meeting_id: int, raw_transcript: str, meeting_name: str):
    """Runs the Claude cleanup step and saves the result. Used both by the
    initial upload pipeline and by manual resubmission from the UI."""
    store.update_status(meeting_id, STATUS_SUMMARIZING)
    cleaned_markdown = clean_up_transcript(raw_transcript, meeting_name)

    out_path = OUTPUT_DIR / f"{meeting_name}_{meeting_id}.md"
    out_path.write_text(f"# {meeting_name}\n\n{cleaned_markdown}\n", encoding="utf-8")
    print(f"Wrote transcript: {out_path}")

    send_email(f"Meeting transcript: {meeting_name}", cleaned_markdown)
    store.save_result(meeting_id, str(out_path))


_SAFE_NAME_CHARS = frozenset("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")


def sanitize_meeting_name(raw_stem: str) -> str:
    """Filenames come from the uploader (device or browser), so strip
    anything that isn't safe to use as a path component before building
    filesystem paths from it."""
    cleaned = "".join(c if c in _SAFE_NAME_CHARS else "_" for c in raw_stem).strip("._")
    return cleaned or f"meeting_{datetime.now():%Y%m%d_%H%M%S}"


# ---------------------------------------------------------------------------
# Job queue: transcription and LLM cleanup are heavy on GPU/VRAM (faster-whisper
# and, if configured, a local Ollama model share the same card). Running them
# inline in the Flask request handler let multiple uploads/retries hit the GPU
# concurrently via Waitress's thread pool, which caused CUDA out-of-memory
# errors. A single background worker processes one job at a time instead;
# endpoints just enqueue and return immediately.
# ---------------------------------------------------------------------------

_job_queue: "queue.Queue[tuple]" = queue.Queue()
_job_in_progress = False  # true while the worker is actively running a job, as opposed to idle-waiting on the queue


def _do_transcribe_and_summarize(meeting_id: int, wav_path: Path, meeting_name: str):
    store.update_status(meeting_id, STATUS_TRANSCRIBING)
    raw_transcript = transcribe_audio(wav_path)
    store.save_raw_transcript(meeting_id, raw_transcript)
    run_summarize_and_save(meeting_id, raw_transcript, meeting_name)


def _do_summarize_only(meeting_id: int, raw_transcript: str, meeting_name: str):
    run_summarize_and_save(meeting_id, raw_transcript, meeting_name)


def _enqueue(job_kind: str, args: tuple):
    _job_queue.put((job_kind, args))
    print(f"Queued '{job_kind}' job for meeting {args[0]} "
          f"({_job_queue.qsize()} waiting behind{' + 1 running' if _job_in_progress else ''})")


def _job_worker():
    global _job_in_progress
    while True:
        job_kind, args = _job_queue.get()
        meeting_id = args[0]
        _job_in_progress = True
        print(f"Starting '{job_kind}' job for meeting {meeting_id}")
        start = time.monotonic()
        try:
            if job_kind == "transcribe":
                _do_transcribe_and_summarize(*args)
            elif job_kind == "summarize":
                _do_summarize_only(*args)
            print(f"Finished '{job_kind}' job for meeting {meeting_id} in {time.monotonic() - start:.1f}s")
        except Exception as e:
            print(f"'{job_kind}' job for meeting {meeting_id} failed after {time.monotonic() - start:.1f}s: {e}")
            traceback.print_exc()
            store.update_status(meeting_id, STATUS_FAILED, error=str(e))
        finally:
            _job_in_progress = False
            _job_queue.task_done()


def queue_length() -> int:
    """Jobs waiting behind the one currently being processed (if any)."""
    waiting = _job_queue.qsize()
    return waiting + 1 if _job_in_progress else waiting


threading.Thread(target=_job_worker, daemon=True).start()


@app.route("/upload", methods=["POST"])
@upload_token_required
def upload():
    if "audio" not in request.files:
        return jsonify({"error": "no 'audio' field in upload"}), 400

    audio_file = request.files["audio"]
    meeting_name = sanitize_meeting_name(Path(audio_file.filename or "").stem)

    upload_tag = f"{datetime.now():%Y%m%d_%H%M%S_%f}"
    wav_path = RAW_AUDIO_DIR / f"{meeting_name}_{upload_tag}.wav"
    audio_file.save(wav_path)
    wav_bytes = wav_path.stat().st_size
    print(f"Received {wav_path} ({wav_bytes} bytes)")

    meeting_id = store.create(meeting_name, str(wav_path), wav_bytes)
    _enqueue("transcribe", (meeting_id, wav_path, meeting_name))

    return jsonify({"status": "ok", "meeting_id": meeting_id}), 200


# ---------------------------------------------------------------------------
# Chunked/resumable upload for the ESP32 device only (the browser recorder
# above sends one small blob and doesn't need this). Meeting recordings can
# run 2+ hours (100+ MB), so a single all-or-nothing POST that has to restart
# from byte 0 on any WiFi hiccup may never complete. Here the server is the
# only source of truth for how many bytes of a given file it already has -
# the device never assumes; it always asks (or reads the previous chunk's
# response) and resumes from that offset, so this also survives the device
# losing power mid-upload, not just a dropped connection.
#
# Responses are plain text, not JSON: the ESP32 client has no JSON parser,
# and the only thing it ever needs from a response is a leading integer
# (bytes received so far) plus an optional trailing "COMPLETE" marker.
# ---------------------------------------------------------------------------

CHUNK_READ_BUFFER = 65536  # bytes read from the request stream per iteration while appending a chunk to disk


def sanitize_upload_filename(raw_name: str) -> str:
    """Like sanitize_meeting_name, but keeps the .wav extension so the
    on-disk chunked-upload filename matches the device's own filename - its
    RTC-timestamped names are already unique, so no extra suffix is needed
    the way the whole-file /upload endpoint above adds one."""
    safe_stem = sanitize_meeting_name(Path(raw_name or "").stem)
    return f"{safe_stem}.wav"


@app.route("/upload/status", methods=["GET"])
@upload_token_required
def upload_status():
    """How many bytes of this filename's chunked upload the server already
    has - 0 if none. The device calls this before (re)starting a file so a
    resumed upload (after a dropped connection, a reboot, or just the next
    periodic retry) knows where to seek to instead of resending from byte 0."""
    name = request.args.get("name", "")
    if not name:
        return "missing 'name'", 400
    safe_name = sanitize_upload_filename(name)
    final_path = RAW_AUDIO_DIR / safe_name
    part_path = RAW_AUDIO_DIR / f"{safe_name}.part"
    if final_path.exists():
        # Already fully received (and finalized) by an earlier attempt whose
        # last ack the device never saw - report it done rather than making
        # the device re-upload something the server already has.
        return str(final_path.stat().st_size), 200, {"Content-Type": "text/plain"}
    if part_path.exists():
        return str(part_path.stat().st_size), 200, {"Content-Type": "text/plain"}
    return "0", 200, {"Content-Type": "text/plain"}


@app.route("/upload/chunk", methods=["POST"])
@upload_token_required
def upload_chunk():
    """Appends one chunk to a resumable upload. The offset the device claims
    (X-Chunk-Offset) must match how many bytes this file already has server-
    side, or the request is rejected with the true offset (409) instead of
    silently appending in the wrong place - that's what makes a retried or
    reordered chunk safe rather than corrupting the partial file."""
    name = request.headers.get("X-File-Name", "")
    if not name:
        return "missing X-File-Name", 400
    try:
        offset = int(request.headers.get("X-Chunk-Offset", ""))
    except ValueError:
        return "missing/invalid X-Chunk-Offset", 400
    is_final = request.headers.get("X-Upload-Final", "") == "1"

    safe_name = sanitize_upload_filename(name)
    final_path = RAW_AUDIO_DIR / safe_name
    part_path = RAW_AUDIO_DIR / f"{safe_name}.part"

    if final_path.exists():
        # Finalized by an earlier attempt already - tell the device it's
        # done instead of erroring on a .part file that no longer exists.
        return f"{final_path.stat().st_size} COMPLETE", 200, {"Content-Type": "text/plain"}

    current_size = part_path.stat().st_size if part_path.exists() else 0
    if offset != current_size:
        return str(current_size), 409, {"Content-Type": "text/plain"}

    with open(part_path, "ab") as f:
        while True:
            piece = request.stream.read(CHUNK_READ_BUFFER)
            if not piece:
                break  # client disconnected mid-chunk - keep whatever landed, device will resume from it
            f.write(piece)
        f.flush()
        os.fsync(f.fileno())  # survive a server crash between chunks, not just an app-level error

    new_size = part_path.stat().st_size

    if not is_final:
        return str(new_size), 200, {"Content-Type": "text/plain"}

    meeting_name = sanitize_meeting_name(Path(safe_name).stem)
    part_path.rename(final_path)
    meeting_id = store.create(meeting_name, str(final_path), new_size)
    _enqueue("transcribe", (meeting_id, final_path, meeting_name))
    print(f"Chunked upload complete: {final_path} ({new_size} bytes)")
    return f"{new_size} COMPLETE", 200, {"Content-Type": "text/plain"}


def wav_duration_seconds(wav_path: str) -> float | None:
    try:
        with wave.open(wav_path, "rb") as f:
            return f.getnframes() / f.getframerate()
    except (OSError, wave.Error):
        return None


def meeting_matches_query(meeting: dict, query: str) -> bool:
    if query in meeting["meeting_name"].lower():
        return True
    if meeting["raw_transcript"] and query in meeting["raw_transcript"].lower():
        return True
    if meeting["transcript_path"]:
        try:
            if query in Path(meeting["transcript_path"]).read_text(encoding="utf-8").lower():
                return True
        except OSError:
            pass
    return False


@app.route("/api/meetings", methods=["GET"])
@login_required
def list_meetings():
    show_archived = request.args.get("archived", "").strip() == "1"
    meetings = store.list_all(include_archived=show_archived)
    if show_archived:
        meetings = [m for m in meetings if m["archived"]]
    query = request.args.get("q", "").strip().lower()
    if query:
        meetings = [m for m in meetings if meeting_matches_query(m, query)]
    for m in meetings:
        m["duration_seconds"] = wav_duration_seconds(m["wav_path"])
    return jsonify(meetings)


@app.route("/api/stats", methods=["GET"])
@login_required
def stats():
    result = store.stats()
    result["queue_length"] = queue_length()
    return jsonify(result)


@app.route("/api/meetings/<int:meeting_id>/archive", methods=["POST"])
@login_required
def archive_meeting(meeting_id):
    meeting = store.get(meeting_id)
    if not meeting:
        abort(404)
    store.set_archived(meeting_id, True)
    return jsonify({"status": "ok"}), 200


@app.route("/api/meetings/<int:meeting_id>/unarchive", methods=["POST"])
@login_required
def unarchive_meeting(meeting_id):
    meeting = store.get(meeting_id)
    if not meeting:
        abort(404)
    store.set_archived(meeting_id, False)
    return jsonify({"status": "ok"}), 200


@app.route("/api/meetings/<int:meeting_id>/retranscribe", methods=["POST"])
@login_required
def retranscribe_meeting(meeting_id):
    """Re-runs Whisper on the already-uploaded WAV. Useful when transcription
    itself failed (e.g. CUDA out of memory) before a raw transcript was ever
    saved, so the normal /retry (Claude/Ollama-only) endpoint has nothing to
    resubmit."""
    meeting = store.get(meeting_id)
    if not meeting:
        abort(404)
    wav_path = Path(meeting["wav_path"])
    if not wav_path.exists():
        return jsonify({"error": "original WAV file is no longer on disk - cannot retranscribe"}), 400

    store.update_status(meeting_id, STATUS_TRANSCRIBING)
    _enqueue("transcribe", (meeting_id, wav_path, meeting["meeting_name"]))
    return jsonify({"status": "ok"}), 200


@app.route("/api/meetings/<int:meeting_id>/retry", methods=["POST"])
@login_required
def retry_meeting(meeting_id):
    """Resubmits the already-transcribed raw text to Claude again, without
    re-running Whisper. Useful after a transient API failure, a rate limit,
    or just to regenerate the summary."""
    meeting = store.get(meeting_id)
    if not meeting:
        abort(404)
    if not meeting["raw_transcript"]:
        return jsonify({"error": "no raw transcript available to resubmit - re-upload the audio"}), 400

    store.update_status(meeting_id, STATUS_SUMMARIZING)
    _enqueue("summarize", (meeting_id, meeting["raw_transcript"], meeting["meeting_name"]))
    return jsonify({"status": "ok"}), 200


@app.route("/api/meetings/<int:meeting_id>/transcript", methods=["GET"])
@login_required
def get_transcript(meeting_id):
    meeting = store.get(meeting_id)
    if not meeting or not meeting["transcript_path"]:
        abort(404)
    path = Path(meeting["transcript_path"])
    if not path.exists():
        abort(404)
    return send_file(path, mimetype="text/markdown")


@app.route("/api/meetings/<int:meeting_id>/raw_transcript", methods=["GET"])
@login_required
def get_raw_transcript(meeting_id):
    meeting = store.get(meeting_id)
    if not meeting or not meeting["raw_transcript"]:
        abort(404)
    return meeting["raw_transcript"], 200, {"Content-Type": "text/plain; charset=utf-8"}


@app.route("/api/meetings/<int:meeting_id>/transcript", methods=["PUT"])
@login_required
def save_transcript(meeting_id):
    meeting = store.get(meeting_id)
    if not meeting or not meeting["transcript_path"]:
        abort(404)
    data = request.get_json(silent=True) or {}
    text = data.get("text")
    if text is None:
        return jsonify({"error": "missing 'text' field"}), 400

    path = Path(meeting["transcript_path"])
    path.write_text(text, encoding="utf-8")
    return jsonify({"status": "ok"}), 200


@app.route("/api/meetings/<int:meeting_id>/email", methods=["POST"])
@login_required
def email_transcript(meeting_id):
    meeting = store.get(meeting_id)
    if not meeting or not meeting["transcript_path"]:
        abort(404)

    data = request.get_json(silent=True) or {}
    to_address = (data.get("to") or "").strip()
    if not to_address or "@" not in to_address:
        return jsonify({"error": "a valid 'to' email address is required"}), 400

    text = data.get("text")
    if text is None:
        path = Path(meeting["transcript_path"])
        if not path.exists():
            abort(404)
        text = path.read_text(encoding="utf-8")

    try:
        send_email_to(to_address, f"Meeting transcript: {meeting['meeting_name']}", text)
        return jsonify({"status": "ok"}), 200
    except Exception as e:
        traceback.print_exc()
        return jsonify({"error": str(e)}), 500


@app.route("/", methods=["GET"])
@login_required
def dashboard():
    return render_template("dashboard.html")


@app.route("/health", methods=["GET"])
def health():
    return jsonify({"status": "ok"}), 200


if __name__ == "__main__":
    print(f"AI Meeting Buddy receiver listening on port {UPLOAD_PORT}")
    print(f"Transcripts will be saved to: {OUTPUT_DIR.resolve()}")
    print(f"Dashboard: http://localhost:{UPLOAD_PORT}/")
    if BEHIND_HTTPS_PROXY:
        # Flask's built-in dev server isn't meant for production traffic;
        # waitress is a production-ready pure-Python WSGI server with no
        # extra system dependencies, so it works the same on Docker/Linux
        # and Windows.
        from waitress import serve
        serve(app, host="0.0.0.0", port=UPLOAD_PORT)
    else:
        app.run(host="0.0.0.0", port=UPLOAD_PORT)
