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

Run:
    pip install -r requirements.txt
    cp .env.example .env   # then fill in ANTHROPIC_API_KEY at minimum
    python receive_and_transcribe.py
"""

import os
import smtplib
import traceback
from datetime import datetime
from email.mime.text import MIMEText
from pathlib import Path

from dotenv import load_dotenv
from flask import Flask, request, jsonify

load_dotenv()

ANTHROPIC_API_KEY = os.environ.get("ANTHROPIC_API_KEY", "")
UPLOAD_PORT = int(os.environ.get("UPLOAD_PORT", "8787"))
OUTPUT_DIR = Path(os.environ.get("OUTPUT_DIR", "./transcripts"))
WHISPER_MODEL = os.environ.get("WHISPER_MODEL", "small")

EMAIL_ENABLED = os.environ.get("EMAIL_ENABLED", "false").lower() == "true"
SMTP_HOST = os.environ.get("SMTP_HOST", "")
SMTP_PORT = int(os.environ.get("SMTP_PORT", "587"))
SMTP_USERNAME = os.environ.get("SMTP_USERNAME", "")
SMTP_PASSWORD = os.environ.get("SMTP_PASSWORD", "")
EMAIL_FROM = os.environ.get("EMAIL_FROM", "")
EMAIL_TO = os.environ.get("EMAIL_TO", "")

OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

app = Flask(__name__)

_whisper_model = None  # lazy-loaded so the server starts fast


def get_whisper_model():
    global _whisper_model
    if _whisper_model is None:
        from faster_whisper import WhisperModel
        print(f"Loading faster-whisper model '{WHISPER_MODEL}' (first run downloads it)...")
        _whisper_model = WhisperModel(WHISPER_MODEL, device="cpu", compute_type="int8")
    return _whisper_model


def transcribe_audio(wav_path: Path) -> str:
    """Local speech-to-text pass. Swap this function's body for a hosted ASR
    API (OpenAI, Deepgram, AssemblyAI, ...) if you'd rather not run Whisper
    locally - nothing else in this file needs to change."""
    model = get_whisper_model()
    segments, _info = model.transcribe(str(wav_path), beam_size=5)
    return " ".join(seg.text.strip() for seg in segments)


def clean_up_with_claude(raw_transcript: str, meeting_name: str) -> str:
    """Sends the raw transcript to Claude for cleanup, summary, and action
    items. This is the step that turns rough ASR output into something
    actually usable - not the transcription itself."""
    if not ANTHROPIC_API_KEY:
        raise RuntimeError("ANTHROPIC_API_KEY is not set in .env")

    import anthropic
    client = anthropic.Anthropic(api_key=ANTHROPIC_API_KEY)

    prompt = f"""You are cleaning up a raw speech-to-text transcript of a meeting called "{meeting_name}".
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

    response = client.messages.create(
        model="claude-sonnet-5",
        max_tokens=4096,
        messages=[{"role": "user", "content": prompt}],
    )
    return response.content[0].text


def send_email(subject: str, body_markdown: str):
    if not EMAIL_ENABLED:
        return
    if not (SMTP_HOST and SMTP_USERNAME and SMTP_PASSWORD and EMAIL_FROM and EMAIL_TO):
        print("EMAIL_ENABLED is true but SMTP settings are incomplete - skipping email.")
        return

    msg = MIMEText(body_markdown, "plain", "utf-8")
    msg["Subject"] = subject
    msg["From"] = EMAIL_FROM
    msg["To"] = EMAIL_TO

    with smtplib.SMTP(SMTP_HOST, SMTP_PORT) as server:
        server.starttls()
        server.login(SMTP_USERNAME, SMTP_PASSWORD)
        server.sendmail(EMAIL_FROM, [EMAIL_TO], msg.as_string())


@app.route("/upload", methods=["POST"])
def upload():
    if "audio" not in request.files:
        return jsonify({"error": "no 'audio' field in upload"}), 400

    audio_file = request.files["audio"]
    meeting_name = Path(audio_file.filename).stem or f"meeting_{datetime.now():%Y%m%d_%H%M%S}"

    raw_dir = OUTPUT_DIR / "raw_audio"
    raw_dir.mkdir(exist_ok=True)
    wav_path = raw_dir / f"{meeting_name}.wav"
    audio_file.save(wav_path)
    print(f"Received {wav_path} ({wav_path.stat().st_size} bytes)")

    try:
        raw_transcript = transcribe_audio(wav_path)
        cleaned_markdown = clean_up_with_claude(raw_transcript, meeting_name)

        out_path = OUTPUT_DIR / f"{meeting_name}.md"
        out_path.write_text(f"# {meeting_name}\n\n{cleaned_markdown}\n", encoding="utf-8")
        print(f"Wrote transcript: {out_path}")

        send_email(f"Meeting transcript: {meeting_name}", cleaned_markdown)

        return jsonify({"status": "ok", "transcript_path": str(out_path)}), 200

    except Exception as e:
        traceback.print_exc()
        return jsonify({"error": str(e)}), 500


@app.route("/health", methods=["GET"])
def health():
    return jsonify({"status": "ok"}), 200


if __name__ == "__main__":
    print(f"AI Meeting Buddy receiver listening on port {UPLOAD_PORT}")
    print(f"Transcripts will be saved to: {OUTPUT_DIR.resolve()}")
    app.run(host="0.0.0.0", port=UPLOAD_PORT)
