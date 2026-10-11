"""Second-brain layer: structured memory per note, dated tasks/events,
retrieval (keyword + optional embeddings), agenda, briefings and recaps.

Everything here is independent of Flask and of which LLM is configured: the
caller passes in an `llm(prompt, json_mode=False) -> str` callable and, for
semantic search, an `embed(text) -> list[float] | None` callable. If either
is missing or fails, the feature degrades (keyword search, deterministic
briefings) instead of failing the pipeline.

Dates are never trusted to the model: it only reports the calendar day and
clock time it heard, and the conversion to UTC happens here, against the
configured local timezone.
"""

import json
import logging
import math
import os
import re
import sqlite3
import struct
from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
from pathlib import Path

try:
    from zoneinfo import ZoneInfo
except ImportError:  # pragma: no cover
    ZoneInfo = None


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def local_tz():
    name = os.environ.get("LOCAL_TIMEZONE", "").strip()
    if name and ZoneInfo:
        try:
            return ZoneInfo(name)
        except Exception:
            logging.warning(f"LOCAL_TIMEZONE {name!r} is not a known timezone - using UTC")
    return timezone.utc


TASK_TYPES = ("todo", "event", "reminder")

# Cap on text sent to the model for memory extraction. The cleaned markdown is
# used (it is far shorter than the raw transcript of a long recording), so a
# two-hour meeting still fits.
MEMORY_MAX_CHARS = int(os.environ.get("MEMORY_MAX_CHARS", "24000"))


# ---------------------------------------------------------------------------
# Time helpers
# ---------------------------------------------------------------------------

def parse_utc(value):
    if not value:
        return None
    try:
        dt = datetime.fromisoformat(str(value).replace("Z", "+00:00"))
    except ValueError:
        return None
    return dt if dt.tzinfo else dt.replace(tzinfo=timezone.utc)


def capture_time_utc(meeting: dict) -> datetime:
    """When a note was recorded, as an aware UTC datetime.

    Device files are named note_<n>_<YYYYMMDDHHMMSS>.wav from the RTC's UTC
    clock; the older MEETING_<date>_<time> names are device-local wall time;
    anything else falls back to when the server received it."""
    name = meeting.get("meeting_name") or ""
    m = re.search(r"note_\d+_(\d{14})", name)
    if m:
        try:
            return datetime.strptime(m.group(1), "%Y%m%d%H%M%S").replace(tzinfo=timezone.utc)
        except ValueError:
            pass
    if name.startswith("MEETING_") and len(name) >= 23:
        try:
            naive = datetime.strptime(name[8:23], "%Y%m%d_%H%M%S")
            return naive.replace(tzinfo=local_tz()).astimezone(timezone.utc)
        except ValueError:
            pass
    return parse_utc(meeting.get("received_at")) or datetime.now(timezone.utc)


_RELATIVE_DAYS = {
    "day after tomorrow": 2, "übermorgen": 2, "tomorrow": 1, "morgen": 1,
    "today": 0, "heute": 0, "tonight": 0, "heute abend": 0,
}


def resolve_due(date_text, time_text, original_text, captured_utc, tz):
    """(due_utc_iso | None, precision) from the model's reported local date
    and time. Relative words are re-derived from the capture date so a model
    that miscounted "tomorrow" cannot move the task to the wrong day."""
    capture_local = captured_utc.astimezone(tz)
    day = None
    spoken = (original_text or "").lower()
    # Longest phrases first so "day after tomorrow" is not read as "tomorrow".
    for phrase in sorted(_RELATIVE_DAYS, key=len, reverse=True):
        if re.search(r"\b" + re.escape(phrase) + r"\b", spoken):
            day = (capture_local + timedelta(days=_RELATIVE_DAYS[phrase])).date()
            break
    if day is None:
        try:
            day = datetime.strptime((date_text or "").strip(), "%Y-%m-%d").date()
        except ValueError:
            return None, "none"
    if abs((day - capture_local.date()).days) > 3650:
        return None, "none"

    clock = None
    if time_text:
        try:
            clock = datetime.strptime(time_text.strip()[:5], "%H:%M").time()
        except ValueError:
            clock = None
    if clock is None:
        local_dt = datetime(day.year, day.month, day.day, tzinfo=tz)
        return local_dt.astimezone(timezone.utc).isoformat(timespec="seconds"), "day"
    local_dt = datetime(day.year, day.month, day.day, clock.hour, clock.minute, tzinfo=tz)
    return local_dt.astimezone(timezone.utc).isoformat(timespec="seconds"), "time"


# ---------------------------------------------------------------------------
# JSON handling
# ---------------------------------------------------------------------------

def extract_json(text: str):
    """First JSON object in a model reply, tolerating code fences and prose."""
    if not text:
        return None
    text = text.strip()
    fence = re.search(r"```(?:json)?\s*(.*?)```", text, re.DOTALL)
    if fence:
        text = fence.group(1).strip()
    start = text.find("{")
    if start < 0:
        return None
    depth = 0
    in_str = False
    esc = False
    for i in range(start, len(text)):
        c = text[i]
        if in_str:
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                in_str = False
        elif c == '"':
            in_str = True
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                try:
                    return json.loads(text[start:i + 1])
                except ValueError:
                    return None
    return None


def _str_list(value, limit=30):
    if not isinstance(value, list):
        return []
    out = []
    for item in value:
        s = str(item).strip()
        if s and s not in out:
            out.append(s[:300])
        if len(out) >= limit:
            break
    return out


# ---------------------------------------------------------------------------
# Memory extraction
# ---------------------------------------------------------------------------

def build_memory_prompt(text: str, meeting: dict, captured_utc: datetime, tz, language: str) -> str:
    local = captured_utc.astimezone(tz)
    calendar = ", ".join(
        f"{(local + timedelta(days=d)).strftime('%Y-%m-%d %A')} ({'today' if d == 0 else f'{d:+d}'})"
        for d in range(-1, 8)
    )
    lang_line = (f"Write every human-readable field in {language}." if language
                 else "Write every human-readable field in the language of the note.")
    return f"""Turn this private voice note into durable second-brain memory.
Preserve every fact and the original meaning. Never infer unsupported details.

Reply with ONE JSON object and nothing else, with exactly these keys:
  "title": friendly headline of 3-7 words, max 60 characters, naming the actual subject
           (e.g. "Portugal partnership cost discussion"); no dates, quotes or trailing punctuation (string)
  "summary": compact factual summary (string)
  "importance": 0 fleeting, 1 normal, 2 important, 3 critical (integer)
  "topics", "people", "projects", "decisions", "facts", "questions": arrays of short strings
  "tasks": array of objects with keys
       "title": concrete task or appointment title,
       "type": "event" (appointment/meeting/scheduled activity at a concrete time),
               "reminder" (explicit reminder) or "todo" (other actionable commitment),
       "date": local calendar day as YYYY-MM-DD, or "" if none was stated,
       "time": local clock time as HH:MM (24h), or "" if no clock time was stated,
       "original_time_text": the exact temporal phrase spoken, or "",
       "confidence": 0-100 confidence in the date interpretation.

Rules for tasks: only genuine commitments, appointments and reminders. An idea,
wish, observation or "might do" is NOT a task. Something already done is a
fact, not a task. A clock time that is only a deadline ("send it by 5pm") is a
todo, not an event. For vague words (later, soon, sometime) leave date and time
empty. Never invent a date. Resolve weekdays against this calendar of LOCAL
dates: {calendar}.
{lang_line}
Use empty arrays when evidence is absent.

Note name: {meeting.get('meeting_name')}
Tag: {meeting.get('tag')}
Recorded (local): {local.strftime('%Y-%m-%d %H:%M')}

Note:
---
{text[:MEMORY_MAX_CHARS]}
---
"""


def normalize_memory(raw, captured_utc: datetime, tz) -> dict | None:
    """Validated memory dict from the model's JSON, or None if unusable."""
    if not isinstance(raw, dict):
        return None
    title = str(raw.get("title") or "").strip()[:200]
    summary = str(raw.get("summary") or "").strip()[:2000]
    if not title and not summary:
        return None
    try:
        importance = max(0, min(3, int(raw.get("importance", 1))))
    except (TypeError, ValueError):
        importance = 1

    tasks = []
    for t in raw.get("tasks") or []:
        if not isinstance(t, dict):
            continue
        text = str(t.get("title") or t.get("text") or "").strip()[:300]
        if not text:
            continue
        ttype = str(t.get("type") or "todo").strip().lower()
        if ttype not in TASK_TYPES:
            ttype = "todo"
        try:
            confidence = max(0, min(100, int(t.get("confidence", 50))))
        except (TypeError, ValueError):
            confidence = 50
        original = str(t.get("original_time_text") or "").strip()[:120]
        due, precision = (None, "none")
        # A low-confidence date is worse than no date: it would put a wrong
        # entry on the calendar. Keep the task, drop the date.
        if confidence >= 40:
            due, precision = resolve_due(t.get("date"), t.get("time"), original, captured_utc, tz)
        if ttype == "event" and precision != "time":
            ttype = "todo" if precision == "none" else "reminder"
        tasks.append({
            "text": text, "type": ttype, "due_utc": due, "date_precision": precision,
            "confidence": confidence, "original_time_text": original,
        })

    return {
        "title": title or summary[:60],
        "summary": summary,
        "importance": importance,
        "topics": _str_list(raw.get("topics")),
        "people": _str_list(raw.get("people")),
        "projects": _str_list(raw.get("projects")),
        "decisions": _str_list(raw.get("decisions")),
        "facts": _str_list(raw.get("facts")),
        "questions": _str_list(raw.get("questions")),
        "tasks": tasks,
    }


def extract_memory(llm, text: str, meeting: dict, language: str = ""):
    """Runs the extraction. Returns a normalized memory dict, or None."""
    tz = local_tz()
    captured = capture_time_utc(meeting)
    prompt = build_memory_prompt(text, meeting, captured, tz, language)
    for attempt in range(2):
        raw = extract_json(llm(prompt, json_mode=True))
        memory = normalize_memory(raw, captured, tz)
        if memory:
            return memory
        logging.info(f"Memory extraction attempt {attempt + 1} returned unusable JSON")
    return None


# ---------------------------------------------------------------------------
# Store
# ---------------------------------------------------------------------------

def _pack(vec):
    return struct.pack(f"{len(vec)}f", *vec)


def _unpack(blob):
    return list(struct.unpack(f"{len(blob) // 4}f", blob))


def _cosine(a, b):
    if not a or not b or len(a) != len(b):
        return 0.0
    dot = sum(x * y for x, y in zip(a, b))
    na = math.sqrt(sum(x * x for x in a))
    nb = math.sqrt(sum(y * y for y in b))
    return dot / (na * nb) if na and nb else 0.0


class BrainStore:
    def __init__(self, db_path: Path):
        self.db_path = db_path

    @contextmanager
    def _connect(self):
        conn = sqlite3.connect(self.db_path)
        conn.row_factory = sqlite3.Row
        try:
            yield conn
            conn.commit()
        finally:
            conn.close()

    # --- memory -----------------------------------------------------------
    def save_memory(self, meeting_id: int, memory: dict, language: str = ""):
        with self._connect() as conn:
            conn.execute(
                "UPDATE meetings SET title = ?, summary = ?, importance = ?, language = ?, "
                "memory_json = ?, memory_updated_at = ? WHERE id = ?",
                (memory["title"], memory["summary"], memory["importance"], language or None,
                 json.dumps(memory, ensure_ascii=False), _now_iso(), meeting_id),
            )

    def set_title(self, meeting_id: int, title: str):
        """Replaces just the title, keeping the title inside memory_json in
        step so retrieval and the dashboard agree."""
        with self._connect() as conn:
            row = conn.execute("SELECT memory_json FROM meetings WHERE id = ?", (meeting_id,)).fetchone()
            memory_json = row["memory_json"] if row else None
            if memory_json:
                try:
                    memory = json.loads(memory_json)
                    memory["title"] = title
                    memory_json = json.dumps(memory, ensure_ascii=False)
                except ValueError:
                    pass
            conn.execute("UPDATE meetings SET title = ?, memory_json = ? WHERE id = ?",
                         (title, memory_json, meeting_id))

    def get_memory(self, meeting_id: int):
        with self._connect() as conn:
            row = conn.execute("SELECT memory_json FROM meetings WHERE id = ?", (meeting_id,)).fetchone()
        if not row or not row["memory_json"]:
            return None
        try:
            return json.loads(row["memory_json"])
        except ValueError:
            return None

    def list_memories(self, since_utc=None, until_utc=None):
        """Completed, unarchived notes that have a memory, newest first."""
        with self._connect() as conn:
            rows = conn.execute(
                "SELECT id, meeting_name, tag, received_at, status, transcript_path, memory_json "
                "FROM meetings WHERE archived = 0 AND status = 'completed' AND memory_json IS NOT NULL "
                "ORDER BY id DESC"
            ).fetchall()
        out = []
        for r in rows:
            try:
                memory = json.loads(r["memory_json"])
            except ValueError:
                continue
            meeting = dict(r)
            captured = capture_time_utc(meeting)
            if since_utc and captured < since_utc:
                continue
            if until_utc and captured >= until_utc:
                continue
            out.append({"id": r["id"], "name": r["meeting_name"], "tag": r["tag"],
                        "captured_utc": captured.isoformat(timespec="seconds"),
                        "memory": memory, "transcript_path": r["transcript_path"]})
        return out

    def pending_memory_ids(self):
        with self._connect() as conn:
            return [r["id"] for r in conn.execute(
                "SELECT id FROM meetings WHERE status = 'completed' AND memory_json IS NULL "
                "AND transcript_path IS NOT NULL ORDER BY id")]

    # --- moments ----------------------------------------------------------
    def replace_moments(self, meeting_id: int, offsets_ms):
        now = _now_iso()
        clean = sorted({int(o) for o in offsets_ms if int(o) >= 0})[:500]
        with self._connect() as conn:
            conn.execute("DELETE FROM moments WHERE meeting_id = ?", (meeting_id,))
            conn.executemany(
                "INSERT INTO moments (meeting_id, offset_ms, created_at) VALUES (?, ?, ?)",
                [(meeting_id, o, now) for o in clean])
        return clean

    def get_moments(self, meeting_id: int):
        with self._connect() as conn:
            return [r["offset_ms"] for r in conn.execute(
                "SELECT offset_ms FROM moments WHERE meeting_id = ? ORDER BY offset_ms", (meeting_id,))]

    def moment_counts(self):
        with self._connect() as conn:
            return {r["meeting_id"]: r["n"] for r in conn.execute(
                "SELECT meeting_id, COUNT(*) AS n FROM moments GROUP BY meeting_id")}

    # --- embeddings -------------------------------------------------------
    def save_embedding(self, meeting_id: int, model: str, vector):
        with self._connect() as conn:
            conn.execute(
                "INSERT INTO embeddings (meeting_id, model, vector, updated_at) VALUES (?, ?, ?, ?) "
                "ON CONFLICT(meeting_id) DO UPDATE SET model = excluded.model, "
                "vector = excluded.vector, updated_at = excluded.updated_at",
                (meeting_id, model, _pack(vector), _now_iso()))

    def all_embeddings(self, model: str):
        with self._connect() as conn:
            return {r["meeting_id"]: _unpack(r["vector"]) for r in conn.execute(
                "SELECT meeting_id, vector FROM embeddings WHERE model = ?", (model,))}

    def missing_embedding_ids(self, model: str):
        with self._connect() as conn:
            return [r["id"] for r in conn.execute(
                "SELECT m.id FROM meetings m LEFT JOIN embeddings e "
                "ON e.meeting_id = m.id AND e.model = ? "
                "WHERE m.memory_json IS NOT NULL AND e.meeting_id IS NULL", (model,))]

    # --- tasks ------------------------------------------------------------
    def tasks_between(self, start_utc, end_utc):
        """Open dated tasks/events whose due time is in [start, end)."""
        with self._connect() as conn:
            rows = conn.execute(
                "SELECT todos.*, meetings.meeting_name, meetings.title AS meeting_title, meetings.tag FROM todos "
                "JOIN meetings ON meetings.id = todos.meeting_id "
                "WHERE meetings.archived = 0 AND todos.done = 0 AND todos.due_utc IS NOT NULL "
                "AND todos.due_utc >= ? AND todos.due_utc < ? ORDER BY todos.due_utc",
                (start_utc.isoformat(timespec="seconds"), end_utc.isoformat(timespec="seconds"))).fetchall()
        return [dict(r) for r in rows]

    def open_tasks(self):
        with self._connect() as conn:
            rows = conn.execute(
                "SELECT todos.*, meetings.meeting_name, meetings.title AS meeting_title, meetings.tag FROM todos "
                "JOIN meetings ON meetings.id = todos.meeting_id "
                "WHERE meetings.archived = 0 AND todos.done = 0 ORDER BY todos.id DESC").fetchall()
        return [dict(r) for r in rows]


# ---------------------------------------------------------------------------
# Retrieval
# ---------------------------------------------------------------------------

_STOP = set("""a an and are as at be but by for from has have how i in is it its me my of on or
that the their this to was were what when where which who why will with you your about did do does
der die das und ist ein eine ich mein meine von mit für auf wie was wann wo wer warum""".split())


def query_terms(question: str):
    words = re.findall(r"[\w'-]+", (question or "").lower())
    return [w for w in words if len(w) > 2 and w not in _STOP]


def _searchable(entry) -> str:
    m = entry["memory"]
    parts = [m.get("title", ""), m.get("summary", ""), entry.get("name", "")]
    for key in ("topics", "people", "projects", "decisions", "facts", "questions"):
        parts.extend(m.get(key) or [])
    parts.extend(t["text"] for t in m.get("tasks") or [])
    return " ".join(parts).lower()


def retrieve(question: str, memories, limit=5, query_vec=None, vectors=None):
    """Top memories for a question: keyword score, blended with cosine
    similarity when embeddings exist, plus a small boost for importance and
    a recency tiebreak (memories arrive newest first)."""
    terms = query_terms(question)
    scored = []
    for rank, entry in enumerate(memories):
        hay = _searchable(entry)
        kw = sum(min(hay.count(t), 3) for t in terms)
        sim = _cosine(query_vec, vectors.get(entry["id"])) if (query_vec and vectors) else 0.0
        if kw == 0 and sim < 0.35:
            continue
        score = kw + sim * 10 + entry["memory"].get("importance", 1) * 0.5 - rank * 0.001
        scored.append((score, entry))
    scored.sort(key=lambda s: s[0], reverse=True)
    return [e for _, e in scored[:limit]]


def memory_context(entry, body_excerpt: str = "") -> str:
    m = entry["memory"]
    lines = [f"[note {entry['id']}] {m.get('title')} (recorded {entry['captured_utc']}, tag {entry['tag']})",
             f"Summary: {m.get('summary')}"]
    for label, key in (("Decisions", "decisions"), ("Facts", "facts"), ("People", "people"),
                       ("Open questions", "questions")):
        if m.get(key):
            lines.append(f"{label}: " + "; ".join(m[key]))
    if m.get("tasks"):
        lines.append("Tasks: " + "; ".join(
            t["text"] + (f" (due {t['due_utc']})" if t.get("due_utc") else "") for t in m["tasks"]))
    if body_excerpt:
        lines.append("Excerpt: " + body_excerpt)
    return "\n".join(lines)


def build_ask_prompt(question: str, contexts, now_local: datetime) -> str:
    joined = "\n\n".join(contexts) if contexts else "(no matching notes)"
    return f"""You answer questions about the user's own voice notes. Use ONLY the notes below.
If they do not contain the answer, say you could not find it in the notes. Be concise (at most
four sentences), plain text, no markdown, suitable for being read aloud. Mention which note
number(s) you used like (note 12).

Current local date and time: {now_local.strftime('%Y-%m-%d %H:%M %A')}

Notes:
{joined}

Question: {question}
Answer:"""


# ---------------------------------------------------------------------------
# Agenda, briefings, recap (deterministic - no LLM needed)
# ---------------------------------------------------------------------------

def _spoken_time(due_utc: str, precision: str, tz) -> str:
    dt = parse_utc(due_utc)
    if not dt:
        return ""
    local = dt.astimezone(tz)
    return local.strftime("%H:%M") if precision == "time" else ""


def _task_view(t, tz):
    due = parse_utc(t.get("due_utc"))
    local = due.astimezone(tz) if due else None
    return {
        "id": t["id"], "text": t["text"], "type": t.get("type") or "todo",
        "due_utc": t.get("due_utc"), "date_precision": t.get("date_precision") or "none",
        "local_date": local.strftime("%Y-%m-%d") if local else None,
        "local_time": _spoken_time(t["due_utc"], t.get("date_precision"), tz) if local else "",
        "meeting_id": t["meeting_id"], "done": bool(t["done"]),
    }


def agenda(store: BrainStore, start_local_date, days: int):
    """Dated open tasks/events for `days` local days from start_local_date."""
    tz = local_tz()
    start = datetime(start_local_date.year, start_local_date.month, start_local_date.day, tzinfo=tz)
    end = start + timedelta(days=days)
    return [_task_view(t, tz) for t in store.tasks_between(
        start.astimezone(timezone.utc), end.astimezone(timezone.utc))]


def build_briefing(store: BrainStore, period: str, now_utc: datetime | None = None) -> dict:
    """Daily or weekly briefing as structured data plus speakable text."""
    tz = local_tz()
    now = (now_utc or datetime.now(timezone.utc)).astimezone(tz)
    days = 7 if period == "weekly" else 1
    items = agenda(store, now.date(), days)
    events = [i for i in items if i["type"] == "event"]
    tasks = [i for i in items if i["type"] != "event"]

    overdue = []
    horizon = datetime(now.year, now.month, now.day, tzinfo=tz).astimezone(timezone.utc)
    for t in store.open_tasks():
        if t.get("due_utc") and (parse_utc(t["due_utc"]) or horizon) < horizon:
            overdue.append(_task_view(t, tz))

    def describe(i, with_day):
        when = []
        if with_day and i["local_date"]:
            when.append(datetime.strptime(i["local_date"], "%Y-%m-%d").strftime("%A"))
        if i["local_time"]:
            when.append("at " + i["local_time"])
        return f"{i['text']}{(' ' + ' '.join(when)) if when else ''}"

    label = "this week" if period == "weekly" else "today"
    parts = []
    if events:
        parts.append(f"You have {len(events)} appointment{'s' if len(events) != 1 else ''} {label}: "
                     + "; ".join(describe(e, period == "weekly") for e in events) + ".")
    if tasks:
        parts.append(f"{len(tasks)} task{'s' if len(tasks) != 1 else ''} due {label}: "
                     + "; ".join(describe(t, period == "weekly") for t in tasks) + ".")
    if overdue:
        parts.append(f"{len(overdue)} overdue: " + "; ".join(o["text"] for o in overdue[:5]) + ".")
    if not parts:
        parts.append(f"Nothing is scheduled {label}.")
    return {"period": period, "generated_local": now.strftime("%Y-%m-%d %H:%M"),
            "events": events, "tasks": tasks, "overdue": overdue, "text": " ".join(parts)}


def recap_window(period: str, now_utc: datetime | None = None):
    tz = local_tz()
    now = (now_utc or datetime.now(timezone.utc)).astimezone(tz)
    days = {"day": 1, "week": 7, "month": 30}.get(period, 7)
    end = datetime(now.year, now.month, now.day, tzinfo=tz) + timedelta(days=1)
    return (end - timedelta(days=days)).astimezone(timezone.utc), end.astimezone(timezone.utc)


def build_recap_prompt(entries, period: str) -> str:
    blocks = "\n\n".join(memory_context(e) for e in entries)
    return f"""Write a short recap of the user's last {period} from these voice-note memories.
Plain text, at most six sentences: main themes, decisions made, and what is still open.
Use only the notes below.

{blocks}

Recap:"""


# ---------------------------------------------------------------------------
# Voice task commands
# ---------------------------------------------------------------------------

def build_task_command_prompt(speech: str, tasks) -> str:
    listing = "\n".join(f'{t["id"]}: {t["text"]}' for t in tasks) or "(none)"
    return f"""The user spoke a command about their task list. Decide if it asks to mark a
task done ("complete") or reopen it ("reopen"). Match it to exactly one task from the list
by meaning; if it is ambiguous or matches none, use action "none".
Reply with ONE JSON object: {{"action": "complete"|"reopen"|"none", "task_id": <id or null>}}

Tasks:
{listing}

Spoken command: {speech}"""


def apply_task_command(llm, speech: str, open_tasks, done_tasks):
    """Returns (action, task | None). Only ever acts on an id that exists in
    the list the model was shown, so a hallucinated id cannot touch anything."""
    speech = (speech or "").strip()
    if not speech or not open_tasks and not done_tasks:
        return "none", None
    pool = {t["id"]: t for t in (list(open_tasks) + list(done_tasks))}
    shown = list(open_tasks)[:60] + list(done_tasks)[:20]
    result = extract_json(llm(build_task_command_prompt(speech, shown), json_mode=True)) or {}
    action = result.get("action")
    try:
        task = pool.get(int(result.get("task_id")))
    except (TypeError, ValueError):
        task = None
    if action not in ("complete", "reopen") or task is None:
        return "none", None
    if action == "complete" and task["done"]:
        return "none", None
    if action == "reopen" and not task["done"]:
        return "none", None
    return action, task
