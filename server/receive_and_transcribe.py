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
import io
import os
import queue
import secrets
import smtplib
import sys
import threading
import time
import traceback
import wave
import zipfile
from datetime import datetime
from email.mime.text import MIMEText
from pathlib import Path

from dotenv import load_dotenv
from flask import Flask, request, jsonify, render_template, send_file, abort, session, redirect, url_for
from werkzeug.middleware.proxy_fix import ProxyFix
from werkzeug.security import generate_password_hash, check_password_hash

from db import (MeetingStore, STATUS_TRANSCRIBING, STATUS_SUMMARIZING, STATUS_FAILED,
                STATUS_COMPLETED, DEFAULT_TAG)

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

# Tags whose recordings are emailed individually as soon as they finish.
# Everything else is collected into the periodic digest instead. Comma
# separated so a second tag can be promoted to immediate without a code change.
IMMEDIATE_EMAIL_TAGS = {
    t.strip() for t in os.environ.get("IMMEDIATE_EMAIL_TAGS", DEFAULT_TAG).split(",") if t.strip()
}
DIGEST_SUBJECT_PREFIX = os.environ.get("DIGEST_SUBJECT_PREFIX", "Notes digest")

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
    if request.path.startswith("/api/") or request.path in ("/upload", "/upload/status", "/upload/chunk", "/upload/resolve"):
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


# ---------------------------------------------------------------------------
# Tags. A recording's tag decides which set of instructions the LLM gets, so
# a quick voice note isn't forced through the meeting-minutes template. The
# set is fixed here rather than free-form: every tag needs its own prompt to
# be worth having, and the dashboard's filter is built from this same list.
# ---------------------------------------------------------------------------

TAG_INSTRUCTIONS = {
    "Meeting": """Produce a markdown document with these sections, in this order:
1. "## Summary" - a short paragraph on what the meeting was about.
2. "## Key points" - a bulleted list of the substantive points discussed.
3. "## Action items" - a bulleted list of anything that sounds like a task,
   decision, or follow-up someone should act on. If there are none, say so.
4. "## Cleaned transcript" - the transcript rewritten into readable prose
   with filler words and obvious ASR artifacts removed, but without changing
   what was actually said or inventing content that isn't there.""",

    "Note": """This is a short spoken note to self, not a meeting - keep the output
brief and don't pad it out with structure the content doesn't justify.

Produce a markdown document with these sections, in this order:
1. "## Note" - the note rewritten as clear, readable prose, with filler words
   and obvious ASR artifacts removed. Keep it close to the original wording.
2. "## To do" - a bulleted list of anything that is clearly a task or reminder.
   Omit this section entirely if there are none.""",

    "Idea": """This is someone thinking out loud about an idea. Treat it as raw
material to develop, not minutes to record.

Produce a markdown document with these sections, in this order:
1. "## The idea" - a crisp statement of what is actually being proposed,
   in a few sentences.
2. "## What's behind it" - the reasoning, motivation, or problem the idea
   is meant to solve, as the speaker described it.
3. "## Open questions" - a bulleted list of the things left unresolved or
   unanswered. Include only questions the transcript actually raises or
   obviously leaves hanging - do not invent objections.
4. "## Cleaned transcript" - the transcript rewritten into readable prose,
   without changing what was said or adding content.""",

    "Interview": """This is an interview. It has no speaker labels, so infer from
context who is asking and who is answering, and say so only where the
transcript makes it reasonably clear.

Produce a markdown document with these sections, in this order:
1. "## Summary" - a short paragraph on who was interviewed and about what.
2. "## Questions and answers" - the substance organised as question/answer
   pairs, in the order they were covered. Paraphrase questions for brevity
   but keep the answers close to what was actually said.
3. "## Notable quotes" - a bulleted list of a few verbatim lines worth
   keeping. Omit the section if nothing stands out.
4. "## Cleaned transcript" - the transcript rewritten into readable prose,
   without changing what was said or adding content.""",

    "Personal": """This is a personal recording, not work material. Keep the tone
plain and don't turn it into a business document.

Produce a markdown document with these sections, in this order:
1. "## Summary" - a short paragraph on what this recording is about.
2. "## Things to remember" - a bulleted list of anything worth holding onto:
   plans, reminders, names, dates. Omit the section if there are none.
3. "## Cleaned transcript" - the transcript rewritten into readable prose
   with filler words and obvious ASR artifacts removed, but without changing
   what was actually said or inventing content that isn't there.""",

    "Work": """This is work material, but not a meeting - a spoken note about a
task, a status update, or thinking through a work problem. Keep it practical
and don't inflate it into meeting minutes.

Produce a markdown document with these sections, in this order:
1. "## Summary" - a short paragraph on what this is about.
2. "## Action items" - a bulleted list of anything that sounds like a task,
   decision, or follow-up someone should act on. Omit the section if there
   are none.
3. "## Cleaned transcript" - the transcript rewritten into readable prose
   with filler words and obvious ASR artifacts removed, but without changing
   what was actually said or inventing content that isn't there.""",

    "Buy": """This is a spoken shopping or purchase note. The useful output is the
list itself, not prose about it - keep everything else to a minimum.

Produce a markdown document with these sections, in this order:
1. "## To buy" - a bulleted list of the items mentioned, one per line. Keep
   any quantity, size, brand, or shop the speaker gave alongside the item.
   List only what was actually said - do not add items that would "go with"
   the others.
2. "## Notes" - anything said that is not an item itself (a budget, a
   deadline, where to go). Omit the section if there is nothing.""",
}

TAG_NAMES = list(TAG_INSTRUCTIONS)

# The device and the dashboard may send a tag in any casing; map it back to
# the canonical spelling so "meeting" and "Meeting" aren't two separate tags.
_TAG_BY_LOWER = {name.lower(): name for name in TAG_NAMES}


def normalize_tag(raw_tag: str) -> str | None:
    """Canonical spelling of a supplied tag, or None if it isn't a known tag."""
    return _TAG_BY_LOWER.get((raw_tag or "").strip().lower())


def build_cleanup_prompt(raw_transcript: str, meeting_name: str, tag: str = DEFAULT_TAG) -> str:
    instructions = TAG_INSTRUCTIONS.get(tag) or TAG_INSTRUCTIONS[DEFAULT_TAG]
    return f"""You are cleaning up a raw speech-to-text transcript of a recording called "{meeting_name}",
tagged "{tag}".
The transcript below may contain filler words, run-on sentences, and transcription
errors from an automatic speech recognizer - it has no speaker labels.

{instructions}

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


def clean_up_transcript(raw_transcript: str, meeting_name: str, tag: str = DEFAULT_TAG) -> str:
    """Turns the rough ASR transcript into a cleaned-up markdown summary,
    via either Claude or a local Ollama model depending on LLM_PROVIDER.
    The tag selects which set of instructions the model is given."""
    print(f"Cleaning up '{meeting_name}' [{tag}] with {LLM_PROVIDER} ({len(raw_transcript)} chars of raw transcript)...")
    prompt = build_cleanup_prompt(raw_transcript, meeting_name, tag)
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


# ---------------------------------------------------------------------------
# Digest: everything that isn't mailed immediately is held until a periodic
# run (cron, every 4 hours) sweeps up what has arrived since. Each recording
# is marked emailed once sent, so the same note is never mailed twice - the
# schedule can be changed, or a run repeated, without duplicating anything.
# ---------------------------------------------------------------------------


def _local_time(iso_ts: str) -> str:
    """Renders a stored UTC timestamp in the server's local time. The stored
    format is what db._now_iso writes; anything unexpected is passed through
    rather than crashing a whole digest over one bad row."""
    try:
        return datetime.fromisoformat(iso_ts).astimezone().strftime("%Y-%m-%d %H:%M")
    except (TypeError, ValueError):
        return iso_ts or "?"


def _read_note_body(meeting: dict) -> str:
    """The finished markdown for one recording, minus the leading "# title"
    line that run_summarize_and_save adds - the digest supplies its own
    heading for each entry, so keeping that line would double it up."""
    path = meeting.get("transcript_path")
    if not path or not Path(path).exists():
        return "_(transcript file missing)_"
    text = Path(path).read_text(encoding="utf-8").strip()
    lines = text.split("\n")
    if lines and lines[0].startswith("# "):
        return "\n".join(lines[1:]).strip()
    return text


def build_digest_body(meetings: list) -> str:
    """One markdown email covering every recording in the digest: a table of
    what's included, then the notes themselves grouped under their tag."""
    by_tag = {}
    for m in meetings:
        by_tag.setdefault(m["tag"] or DEFAULT_TAG, []).append(m)

    counts = ", ".join(f"{tag} x{len(items)}" for tag, items in sorted(by_tag.items()))
    parts = [
        f"{len(meetings)} new note(s) since the last digest: {counts}.",
        "",
        "| Tag | Recorded | Note |",
        "| --- | --- | --- |",
    ]
    for tag, items in sorted(by_tag.items()):
        for m in items:
            # Escape pipes so a note name can't break the table layout.
            name = (m["meeting_name"] or "").replace("|", r"\|")
            parts.append(f"| {tag} | {_local_time(m['received_at'])} | {name} |")

    for tag, items in sorted(by_tag.items()):
        parts += ["", "", f"{'=' * 60}", f"{tag.upper()} ({len(items)})", f"{'=' * 60}"]
        for m in items:
            parts += [
                "",
                f"## {m['meeting_name']}  -  {_local_time(m['received_at'])}",
                "",
                _read_note_body(m),
            ]

    return "\n".join(parts)


def run_digest() -> dict:
    """Emails every completed, not-yet-emailed recording that isn't handled
    by the immediate-send path, as a single grouped message. Safe to call
    repeatedly: with nothing pending it sends nothing and reports so."""
    pending = store.list_pending_digest(exclude_tags=IMMEDIATE_EMAIL_TAGS)
    if not pending:
        print("Digest: nothing new to send.")
        return {"status": "ok", "sent": 0, "meeting_ids": []}

    body = build_digest_body(pending)
    subject = f"{DIGEST_SUBJECT_PREFIX}: {len(pending)} new note(s)"

    if not EMAIL_ENABLED:
        # Without this the notes would be silently marked as emailed and
        # never appear in any future digest.
        print(f"Digest: EMAIL_ENABLED is false - leaving {len(pending)} note(s) pending.")
        return {"status": "skipped", "reason": "email disabled", "sent": 0,
                "meeting_ids": [m["id"] for m in pending]}

    if not (SMTP_HOST and SMTP_USERNAME and SMTP_PASSWORD and EMAIL_FROM and EMAIL_TO):
        # send_email() would return quietly here; marking the notes emailed
        # after that would lose them for good.
        print(f"Digest: SMTP settings incomplete - leaving {len(pending)} note(s) pending.")
        return {"status": "skipped", "reason": "smtp incomplete", "sent": 0,
                "meeting_ids": [m["id"] for m in pending]}

    # Marked only after the send returns; if it raises, the notes stay
    # pending and the next run retries them.
    send_email_to(EMAIL_TO, subject, body)
    ids = [m["id"] for m in pending]
    store.mark_emailed(ids)
    print(f"Digest: emailed {len(ids)} note(s) - ids {ids}")
    return {"status": "ok", "sent": len(ids), "meeting_ids": ids}


def run_summarize_and_save(meeting_id: int, raw_transcript: str, meeting_name: str):
    """Runs the Claude cleanup step and saves the result. Used both by the
    initial upload pipeline and by manual resubmission from the UI.

    The tag is read here rather than passed in, so a job that was queued
    before the tag was set (the device tags right after the upload lands)
    still picks up the final tag when it actually runs."""
    meeting = store.get(meeting_id)
    tag = (meeting or {}).get("tag") or DEFAULT_TAG
    store.update_status(meeting_id, STATUS_SUMMARIZING)
    cleaned_markdown = clean_up_transcript(raw_transcript, meeting_name, tag)

    out_path = OUTPUT_DIR / f"{meeting_name}_{meeting_id}.md"
    out_path.write_text(f"# {meeting_name}\n\n{cleaned_markdown}\n", encoding="utf-8")
    print(f"Wrote transcript: {out_path}")

    # Tags in IMMEDIATE_EMAIL_TAGS go out on their own the moment they are
    # ready; everything else is held back for the periodic digest, so a
    # stream of short notes doesn't become a stream of emails. Marking the
    # immediate ones as emailed is what keeps them out of that digest.
    if tag in IMMEDIATE_EMAIL_TAGS:
        send_email(f"Meeting transcript: {meeting_name}", cleaned_markdown)
        store.mark_emailed([meeting_id])

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


def list_partial_uploads() -> list[dict]:
    """Chunked uploads (see /upload/chunk) that are still in flight - a
    '<name>.wav.part' file sitting in RAW_AUDIO_DIR with more chunks still to
    come. The protocol never tells the server a file's final size upfront, so
    only bytes-received-so-far is available, not a percentage."""
    partials = []
    for part_path in RAW_AUDIO_DIR.glob("*.wav.part"):
        try:
            stat = part_path.stat()
        except OSError:
            continue
        partials.append({
            "name": part_path.name[: -len(".part")],
            "bytes_received": stat.st_size,
            "updated_at": datetime.fromtimestamp(stat.st_mtime, tz=None).isoformat(timespec="seconds"),
        })
    partials.sort(key=lambda p: p["updated_at"], reverse=True)
    return partials


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
    # A tag sent with the upload is applied straight away; uploaders that
    # can't add a form field (the device's chunked path) instead call
    # /api/meetings/<id>/tag afterwards. Either way the recording starts
    # out as DEFAULT_TAG.
    tag = normalize_tag(request.form.get("tag", ""))
    if tag:
        store.set_tag(meeting_id, tag)
    _enqueue("transcribe", (meeting_id, wav_path, meeting_name))

    return jsonify({"status": "ok", "meeting_id": meeting_id, "tag": tag or DEFAULT_TAG}), 200


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
    try:
        declared_len = int(request.headers.get("Content-Length", ""))
    except ValueError:
        return "missing/invalid Content-Length", 400
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

    # Read exactly as many bytes as the client declared, and verify that's
    # what actually arrived before appending anything - a dropped connection
    # or truncated stream must never be allowed to land a short write on
    # disk, because that write would still get ack'd as valid progress
    # (current_size advances), permanently desyncing the device's own
    # offset bookkeeping from what's really on disk for the rest of this
    # file's upload.
    received = bytearray()
    try:
        remaining = declared_len
        while remaining > 0:
            piece = request.stream.read(min(CHUNK_READ_BUFFER, remaining))
            if not piece:
                break  # connection dropped before declared_len bytes arrived
            received.extend(piece)
            remaining -= len(piece)
    except (OSError, ConnectionError):
        pass  # treated the same as a clean short read below

    if len(received) != declared_len:
        return str(current_size), 400, {"Content-Type": "text/plain"}

    with open(part_path, "ab") as f:
        f.write(received)
        f.flush()
        os.fsync(f.fileno())  # survive a server crash between chunks, not just an app-level error

    new_size = part_path.stat().st_size
    if new_size != current_size + declared_len:
        # Another request raced us onto the same .part file, or the OS
        # buffered less than we wrote - don't trust the size, don't finalize.
        return str(new_size), 409, {"Content-Type": "text/plain"}

    if not is_final:
        return str(new_size), 200, {"Content-Type": "text/plain"}

    meeting_name = sanitize_meeting_name(Path(safe_name).stem)
    part_path.rename(final_path)
    meeting_id = store.create(meeting_name, str(final_path), new_size)
    _enqueue("transcribe", (meeting_id, final_path, meeting_name))
    print(f"Chunked upload complete: {final_path} ({new_size} bytes)")
    # The trailing id lets the device follow up with POST /api/meetings/<id>/tag.
    # It comes after "COMPLETE" so an older device that only looks for the
    # leading integer and the marker keeps working unchanged.
    return f"{new_size} COMPLETE {meeting_id}", 200, {"Content-Type": "text/plain"}


@app.route("/upload/resolve", methods=["GET"])
@upload_token_required
def upload_resolve():
    """The meeting id for an already-uploaded filename, whatever pipeline
    stage it's at. The device needs this whenever a resumed upload lands on a
    file an earlier attempt already finalized: it never saw the finalizing
    "COMPLETE <id>" ack, so without a lookup it can neither tag the recording
    nor fetch its text back later. /api/notes deliberately lists only
    completed notes, so it can't answer this - a recording finalized seconds
    ago is still queued or transcribing.

    Plain text (a bare integer) rather than JSON, like the other endpoints on
    this path: the ESP32 clients have no JSON parser."""
    name = request.args.get("name", "")
    if not name:
        return "missing 'name'", 400
    meeting_name = sanitize_meeting_name(Path(sanitize_upload_filename(name)).stem)
    meeting = store.get_by_name(meeting_name)
    if not meeting:
        return "0", 404, {"Content-Type": "text/plain"}
    return str(meeting["id"]), 200, {"Content-Type": "text/plain"}


def wav_duration_seconds(wav_path: str) -> float | None:
    """None rather than an exception for anything unreadable - this is called
    for every row of the meetings listing, so a single truncated or partial
    WAV must not take the whole list down with it. A file cut short mid-header
    raises EOFError (not wave.Error) from the chunk parser, and one with a
    damaged header can report a zero framerate."""
    try:
        with wave.open(wav_path, "rb") as f:
            framerate = f.getframerate()
            if not framerate:
                return None
            return f.getnframes() / framerate
    except (OSError, wave.Error, EOFError):
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
    tag = normalize_tag(request.args.get("tag", ""))
    if tag:
        meetings = [m for m in meetings if (m["tag"] or DEFAULT_TAG) == tag]
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
    result["partial_uploads"] = list_partial_uploads()
    return jsonify(result)


@app.route("/api/tags", methods=["GET"])
@login_required
def list_tags():
    return jsonify(TAG_NAMES)


@app.route("/api/meetings/<int:meeting_id>/tag", methods=["POST"])
@upload_token_required
def set_meeting_tag(meeting_id):
    """Associates a tag with an already-uploaded recording. Split out from
    /upload so the device can keep sending the WAV exactly as it does today
    and follow up with one small request; also used by the dashboard to
    retag a recording after the fact."""
    meeting = store.get(meeting_id)
    if not meeting:
        abort(404)

    data = request.get_json(silent=True) or {}
    raw_tag = data.get("tag", request.form.get("tag", request.args.get("tag", "")))
    tag = normalize_tag(raw_tag)
    if not tag:
        return jsonify({
            "error": f"unknown tag {raw_tag!r} - must be one of: {', '.join(TAG_NAMES)}"
        }), 400

    store.set_tag(meeting_id, tag)
    print(f"Tagged meeting {meeting_id} as '{tag}'")
    return jsonify({"status": "ok", "tag": tag}), 200


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


@app.route("/api/meetings/<int:meeting_id>/audio", methods=["GET"])
@login_required
def get_audio(meeting_id):
    meeting = store.get(meeting_id)
    if not meeting:
        abort(404)
    path = Path(meeting["wav_path"])
    if not path.exists():
        abort(404)
    # conditional=True lets the browser's <audio> element seek via HTTP Range
    # requests instead of downloading the whole WAV before playback can start.
    return send_file(path, mimetype="audio/wav", conditional=True)


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


@app.route("/api/meetings/<int:meeting_id>/transcript/download", methods=["GET"])
@login_required
def download_transcript(meeting_id):
    meeting = store.get(meeting_id)
    if not meeting or not meeting["transcript_path"]:
        abort(404)
    path = Path(meeting["transcript_path"])
    if not path.exists():
        abort(404)
    return send_file(path, mimetype="text/markdown", as_attachment=True,
                      download_name=f"{meeting['meeting_name']}.md")


@app.route("/api/meetings/<int:meeting_id>/raw_transcript", methods=["GET"])
@login_required
def get_raw_transcript(meeting_id):
    meeting = store.get(meeting_id)
    if not meeting or not meeting["raw_transcript"]:
        abort(404)
    return meeting["raw_transcript"], 200, {"Content-Type": "text/plain; charset=utf-8"}


@app.route("/api/meetings/<int:meeting_id>/raw_transcript/download", methods=["GET"])
@login_required
def download_raw_transcript(meeting_id):
    meeting = store.get(meeting_id)
    if not meeting or not meeting["raw_transcript"]:
        abort(404)
    return meeting["raw_transcript"], 200, {
        "Content-Type": "text/plain; charset=utf-8",
        "Content-Disposition": f'attachment; filename="{meeting["meeting_name"]}_raw.txt"',
    }


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


# ---------------------------------------------------------------------------
# Notes API: read access to the *finished* (post-LLM) notes for scripts and
# other clients, as opposed to the /api/meetings routes above which back the
# dashboard and expose pipeline state. Only completed recordings with a
# transcript actually on disk are visible here - a client asking for notes
# wants the processed output, not a half-finished job.
#
# Authenticated with the same X-Upload-Token the device already uses (a
# logged-in browser session works too), so a script needs no login flow.
# ---------------------------------------------------------------------------


def parse_date_arg(value: str):
    """Accepts YYYY-MM-DD or a full ISO timestamp. Returns None for an empty
    value, and raises ValueError for something unparseable so the caller can
    turn it into a 400 rather than silently ignoring a filter the client
    believed was applied."""
    value = (value or "").strip()
    if not value:
        return None
    try:
        return datetime.fromisoformat(value)
    except ValueError:
        raise ValueError(f"invalid date {value!r} - use YYYY-MM-DD or an ISO timestamp")


def note_recorded_at(meeting: dict):
    """When the recording was actually made, taken from the device's
    RTC-stamped filename (MEETING_YYYYMMDD_HHMMSS). Falls back to received_at
    for uploads that didn't come from the device, which are named
    differently."""
    name = meeting["meeting_name"]
    if name.startswith("MEETING_") and len(name) >= 23:
        try:
            return datetime.strptime(name[8:23], "%Y%m%d_%H%M%S")
        except ValueError:
            pass
    try:
        return datetime.fromisoformat(meeting["received_at"]).replace(tzinfo=None)
    except (ValueError, TypeError):
        return None


def note_text(meeting: dict):
    """The processed markdown for a meeting, or None if it isn't on disk."""
    if not meeting["transcript_path"]:
        return None
    try:
        return Path(meeting["transcript_path"]).read_text(encoding="utf-8")
    except OSError:
        return None


def note_summary(meeting: dict) -> dict:
    """Metadata for one note - deliberately without the markdown body, so a
    listing stays small even across hundreds of recordings."""
    recorded = note_recorded_at(meeting)
    return {
        "id": meeting["id"],
        "name": meeting["meeting_name"],
        "tag": meeting["tag"] or DEFAULT_TAG,
        "recorded_at": recorded.isoformat() if recorded else None,
        "received_at": meeting["received_at"],
        "updated_at": meeting["updated_at"],
        "duration_seconds": wav_duration_seconds(meeting["wav_path"]),
        "wav_bytes": meeting["wav_bytes"],
    }


def select_notes():
    """Completed notes matching the request's filters, newest first.
    Raises ValueError on a malformed filter, so a client that mistypes one
    gets a 400 instead of a silently unfiltered result set."""
    meetings = [
        m for m in store.list_all(include_archived=True)
        if m["status"] == STATUS_COMPLETED and m["transcript_path"]
    ]

    if request.args.get("archived", "").strip() != "1":
        meetings = [m for m in meetings if not m["archived"]]

    ids_arg = request.args.get("ids", "").strip()
    if ids_arg:
        try:
            wanted = {int(part) for part in ids_arg.split(",") if part.strip()}
        except ValueError:
            raise ValueError(f"invalid 'ids' {ids_arg!r} - use a comma-separated list of integers")
        meetings = [m for m in meetings if m["id"] in wanted]

    raw_tag = request.args.get("tag", "").strip()
    if raw_tag:
        tag = normalize_tag(raw_tag)
        if not tag:
            raise ValueError(f"unknown tag {raw_tag!r} - must be one of: {', '.join(TAG_NAMES)}")
        meetings = [m for m in meetings if (m["tag"] or DEFAULT_TAG) == tag]

    since = parse_date_arg(request.args.get("since", ""))
    until = parse_date_arg(request.args.get("until", ""))
    if since or until:
        kept = []
        for m in meetings:
            recorded = note_recorded_at(m)
            if recorded is None:
                continue  # can't place it in time - exclude rather than guess
            if since and recorded < since:
                continue
            if until and recorded > until:
                continue
            kept.append(m)
        meetings = kept

    query = request.args.get("q", "").strip().lower()
    if query:
        meetings = [m for m in meetings if meeting_matches_query(m, query)]

    return meetings


@app.route("/api/notes", methods=["GET"])
@upload_token_required
def list_notes():
    """Metadata for the notes matching the filters. Pass include=content to
    get each note's markdown inline instead of fetching them one by one."""
    try:
        meetings = select_notes()
    except ValueError as e:
        return jsonify({"error": str(e)}), 400

    include_content = request.args.get("include", "").strip() == "content"
    notes = []
    for m in meetings:
        note = note_summary(m)
        if include_content:
            text = note_text(m)
            if text is None:
                continue  # transcript vanished from disk since it completed
            note["content"] = text
        notes.append(note)
    return jsonify({"count": len(notes), "notes": notes})


@app.route("/api/notes/<int:meeting_id>", methods=["GET"])
@upload_token_required
def get_note(meeting_id):
    """One note's processed markdown. format=json wraps it with the same
    metadata the listing returns; the default is the raw markdown, and
    download=1 makes a browser save it as a .md file."""
    meeting = store.get(meeting_id)
    if not meeting or meeting["status"] != STATUS_COMPLETED:
        abort(404)
    text = note_text(meeting)
    if text is None:
        abort(404)

    if request.args.get("format", "").strip() == "json":
        note = note_summary(meeting)
        note["content"] = text
        return jsonify(note)

    headers = {"Content-Type": "text/markdown; charset=utf-8"}
    if request.args.get("download", "").strip() == "1":
        headers["Content-Disposition"] = f'attachment; filename="{meeting["meeting_name"]}.md"'
    return text, 200, headers


@app.route("/api/notes/download", methods=["GET"])
@upload_token_required
def download_notes():
    """Every note matching the filters, bundled as one zip. Built in memory
    rather than through a temp file - notes are markdown, so even hundreds of
    them come to a few MB at most."""
    try:
        meetings = select_notes()
    except ValueError as e:
        return jsonify({"error": str(e)}), 400
    if not meetings:
        return jsonify({"error": "no notes match the given filters"}), 404

    buffer = io.BytesIO()
    used_names = set()
    written = 0
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as archive:
        for m in meetings:
            text = note_text(m)
            if text is None:
                continue
            # Grouped by tag inside the zip, with the id in the filename so
            # two recordings sharing a name can't collide.
            tag_folder = m["tag"] or DEFAULT_TAG
            entry = f"{tag_folder}/{m['meeting_name']}_{m['id']}.md"
            if entry in used_names:
                continue
            used_names.add(entry)
            archive.writestr(entry, text)
            written += 1

    if not written:
        return jsonify({"error": "matching notes are no longer on disk"}), 404

    buffer.seek(0)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    return send_file(buffer, mimetype="application/zip", as_attachment=True,
                     download_name=f"notes_{stamp}.zip")


@app.route("/", methods=["GET"])
@login_required
def dashboard():
    return render_template("dashboard.html")


@app.route("/api/digest", methods=["POST"])
@login_required
def trigger_digest():
    """Runs the digest on demand. The scheduled path is the --digest CLI flag
    below; this exists so a run can be triggered/tested without shell access."""
    try:
        return jsonify(run_digest()), 200
    except Exception as e:
        traceback.print_exc()
        return jsonify({"error": str(e)}), 500


@app.route("/health", methods=["GET"])
def health():
    return jsonify({"status": "ok"}), 200


if __name__ == "__main__":
    # Cron entry point: runs one digest in this process and exits, without
    # starting the web server. Deliberately a mode of the same script so it
    # shares the .env, the database and the email settings.
    if "--digest" in sys.argv:
        result = run_digest()
        sys.exit(0 if result["status"] in ("ok", "skipped") else 1)

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
