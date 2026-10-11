"""SQLite-backed tracking of received meeting recordings and their pipeline state."""

import sqlite3
from contextlib import contextmanager
from datetime import datetime, timezone
from pathlib import Path


def _now_iso() -> str:
    """UTC timestamp with an explicit offset, so the browser (new Date(...))
    converts it to the viewer's local timezone instead of misreading a naive
    string as if it were already local time."""
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


STATUS_RECEIVED = "received"
STATUS_TRANSCRIBING = "transcribing"
STATUS_SUMMARIZING = "summarizing"
STATUS_COMPLETED = "completed"
STATUS_FAILED = "failed"

DEFAULT_TAG = "Meeting"

_SCHEMA = """
CREATE TABLE IF NOT EXISTS meetings (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    meeting_name TEXT NOT NULL,
    status TEXT NOT NULL,
    received_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    wav_path TEXT NOT NULL,
    wav_bytes INTEGER,
    raw_transcript TEXT,
    transcript_path TEXT,
    error TEXT,
    archived INTEGER NOT NULL DEFAULT 0,
    emailed_at TEXT,
    tag TEXT NOT NULL DEFAULT '""" + DEFAULT_TAG + """'
);
"""

_TODOS_SCHEMA = """
CREATE TABLE IF NOT EXISTS todos (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    meeting_id INTEGER NOT NULL,
    text TEXT NOT NULL,
    done INTEGER NOT NULL DEFAULT 0,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    FOREIGN KEY (meeting_id) REFERENCES meetings (id)
);
"""


def init_db(db_path: Path):
    db_path.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(db_path)
    conn.execute(_SCHEMA)
    conn.execute(_TODOS_SCHEMA)
    columns = {row[1] for row in conn.execute("PRAGMA table_info(meetings)")}
    if "archived" not in columns:
        conn.execute("ALTER TABLE meetings ADD COLUMN archived INTEGER NOT NULL DEFAULT 0")
    if "tag" not in columns:
        conn.execute(
            f"ALTER TABLE meetings ADD COLUMN tag TEXT NOT NULL DEFAULT '{DEFAULT_TAG}'"
        )
    if "emailed_at" not in columns:
        conn.execute("ALTER TABLE meetings ADD COLUMN emailed_at TEXT")
        # Backfill every pre-existing recording as already sent. Without this
        # the first digest run would mail out the entire back catalogue in one
        # go, which is exactly the repeated-notes problem the digest exists to
        # avoid.
        conn.execute("UPDATE meetings SET emailed_at = ?", (_now_iso(),))
    if "apple_exported_at" not in columns:
        conn.execute("ALTER TABLE meetings ADD COLUMN apple_exported_at TEXT")
    for name, ddl in _MEETING_MEMORY_COLUMNS:
        if name not in columns:
            conn.execute(f"ALTER TABLE meetings ADD COLUMN {name} {ddl}")
    todo_columns = {row[1] for row in conn.execute("PRAGMA table_info(todos)")}
    for name, ddl in _TODO_COLUMNS:
        if name not in todo_columns:
            conn.execute(f"ALTER TABLE todos ADD COLUMN {name} {ddl}")
    conn.execute(_MOMENTS_SCHEMA)
    conn.execute(_EMBEDDINGS_SCHEMA)
    conn.commit()
    conn.close()


# Structured "memory" extracted from each note (see brain.py). All nullable /
# defaulted so existing rows and the existing pipeline keep working untouched.
_MEETING_MEMORY_COLUMNS = [
    ("title", "TEXT"),
    ("summary", "TEXT"),
    ("importance", "INTEGER NOT NULL DEFAULT 1"),
    ("language", "TEXT"),
    ("memory_json", "TEXT"),
    ("memory_updated_at", "TEXT"),
]

_TODO_COLUMNS = [
    ("due_utc", "TEXT"),
    ("type", "TEXT NOT NULL DEFAULT 'todo'"),
    ("date_precision", "TEXT NOT NULL DEFAULT 'none'"),
    ("confidence", "INTEGER"),
    ("original_time_text", "TEXT"),
    ("apple_exported_at", "TEXT"),
]

_MOMENTS_SCHEMA = """
CREATE TABLE IF NOT EXISTS moments (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    meeting_id INTEGER NOT NULL,
    offset_ms INTEGER NOT NULL,
    created_at TEXT NOT NULL,
    FOREIGN KEY (meeting_id) REFERENCES meetings (id)
);
"""

_EMBEDDINGS_SCHEMA = """
CREATE TABLE IF NOT EXISTS embeddings (
    meeting_id INTEGER PRIMARY KEY,
    model TEXT NOT NULL,
    vector BLOB NOT NULL,
    updated_at TEXT NOT NULL,
    FOREIGN KEY (meeting_id) REFERENCES meetings (id)
);
"""


class MeetingStore:
    def __init__(self, db_path: Path):
        self.db_path = db_path
        init_db(db_path)

    @contextmanager
    def _connect(self):
        conn = sqlite3.connect(self.db_path)
        conn.row_factory = sqlite3.Row
        try:
            yield conn
            conn.commit()
        finally:
            conn.close()

    def create(self, meeting_name: str, wav_path: str, wav_bytes: int = None) -> int:
        now = _now_iso()
        with self._connect() as conn:
            cur = conn.execute(
                "INSERT INTO meetings (meeting_name, status, received_at, updated_at, wav_path, wav_bytes) "
                "VALUES (?, ?, ?, ?, ?, ?)",
                (meeting_name, STATUS_RECEIVED, now, now, wav_path, wav_bytes),
            )
            return cur.lastrowid

    def update_status(self, meeting_id: int, status: str, error: str = None):
        now = _now_iso()
        with self._connect() as conn:
            conn.execute(
                "UPDATE meetings SET status = ?, updated_at = ?, error = ? WHERE id = ?",
                (status, now, error, meeting_id),
            )

    def save_raw_transcript(self, meeting_id: int, raw_transcript: str):
        now = _now_iso()
        with self._connect() as conn:
            conn.execute(
                "UPDATE meetings SET raw_transcript = ?, updated_at = ? WHERE id = ?",
                (raw_transcript, now, meeting_id),
            )

    def save_result(self, meeting_id: int, transcript_path: str):
        now = _now_iso()
        with self._connect() as conn:
            conn.execute(
                "UPDATE meetings SET status = ?, transcript_path = ?, updated_at = ?, error = NULL WHERE id = ?",
                (STATUS_COMPLETED, transcript_path, now, meeting_id),
            )

    def get(self, meeting_id: int):
        with self._connect() as conn:
            row = conn.execute("SELECT * FROM meetings WHERE id = ?", (meeting_id,)).fetchone()
            return dict(row) if row else None

    def get_by_name(self, meeting_name: str):
        """The most recent recording stored under this exact meeting_name, at
        any pipeline status. The device needs this to recover the id of an
        upload whose finalizing ack it never saw: /api/notes only lists
        recordings that have finished transcribing, so a just-finalized one
        is structurally absent from it. Newest first, because the whole-file
        /upload endpoint suffixes its names with a timestamp and can only
        collide across separate uploads, never within one."""
        with self._connect() as conn:
            row = conn.execute(
                "SELECT * FROM meetings WHERE meeting_name = ? ORDER BY id DESC LIMIT 1",
                (meeting_name,),
            ).fetchone()
            return dict(row) if row else None

    def list_all(self, include_archived: bool = False):
        with self._connect() as conn:
            if include_archived:
                rows = conn.execute("SELECT * FROM meetings ORDER BY id DESC").fetchall()
            else:
                rows = conn.execute(
                    "SELECT * FROM meetings WHERE archived = 0 ORDER BY id DESC"
                ).fetchall()
            return [dict(r) for r in rows]

    def set_tag(self, meeting_id: int, tag: str):
        now = _now_iso()
        with self._connect() as conn:
            conn.execute(
                "UPDATE meetings SET tag = ?, updated_at = ? WHERE id = ?",
                (tag, now, meeting_id),
            )

    def mark_emailed(self, meeting_ids):
        """Records that these recordings have been mailed out, so a later
        digest run doesn't send them again. updated_at is deliberately left
        alone: being emailed isn't a change to the recording itself, and
        bumping it would reorder the dashboard for a non-event."""
        ids = list(meeting_ids)
        if not ids:
            return
        now = _now_iso()
        placeholders = ",".join("?" * len(ids))
        with self._connect() as conn:
            conn.execute(
                f"UPDATE meetings SET emailed_at = ? WHERE id IN ({placeholders})",
                [now, *ids],
            )

    def mark_apple_exported(self, meeting_ids):
        """Records that the Apple Shortcut has been handed these notes, so the
        next sync only returns new ones. updated_at is left alone, as with
        mark_emailed."""
        ids = list(meeting_ids)
        if not ids:
            return
        now = _now_iso()
        placeholders = ",".join("?" * len(ids))
        with self._connect() as conn:
            conn.execute(
                f"UPDATE meetings SET apple_exported_at = ? WHERE id IN ({placeholders})",
                [now, *ids],
            )

    def clear_apple_exported(self):
        """Forgets every Apple export so the next sync returns everything."""
        with self._connect() as conn:
            conn.execute("UPDATE meetings SET apple_exported_at = NULL")

    def list_pending_digest(self, exclude_tags=()):
        """Completed, unarchived recordings that have never been emailed,
        oldest first so the digest reads in the order things were recorded.

        exclude_tags carries the tags that are mailed individually the moment
        they finish (Meeting), which must not also appear in the digest."""
        clauses = ["archived = 0", "status = ?", "emailed_at IS NULL"]
        params = [STATUS_COMPLETED]
        excluded = list(exclude_tags)
        if excluded:
            clauses.append(f"tag NOT IN ({','.join('?' * len(excluded))})")
            params.extend(excluded)
        sql = f"SELECT * FROM meetings WHERE {' AND '.join(clauses)} ORDER BY id ASC"
        with self._connect() as conn:
            return [dict(r) for r in conn.execute(sql, params).fetchall()]

    def set_archived(self, meeting_id: int, archived: bool):
        now = _now_iso()
        with self._connect() as conn:
            conn.execute(
                "UPDATE meetings SET archived = ?, updated_at = ? WHERE id = ?",
                (1 if archived else 0, now, meeting_id),
            )

    def stats(self):
        with self._connect() as conn:
            total = conn.execute(
                "SELECT COUNT(*) FROM meetings WHERE archived = 0"
            ).fetchone()[0]
            completed = conn.execute(
                "SELECT COUNT(*) FROM meetings WHERE archived = 0 AND status = ?", (STATUS_COMPLETED,)
            ).fetchone()[0]
            failed = conn.execute(
                "SELECT COUNT(*) FROM meetings WHERE archived = 0 AND status = ?", (STATUS_FAILED,)
            ).fetchone()[0]
            in_progress = conn.execute(
                "SELECT COUNT(*) FROM meetings WHERE archived = 0 AND status IN (?, ?, ?)",
                (STATUS_RECEIVED, STATUS_TRANSCRIBING, STATUS_SUMMARIZING),
            ).fetchone()[0]
            archived = conn.execute(
                "SELECT COUNT(*) FROM meetings WHERE archived = 1"
            ).fetchone()[0]
            total_bytes = conn.execute(
                "SELECT COALESCE(SUM(wav_bytes), 0) FROM meetings WHERE archived = 0"
            ).fetchone()[0]
            tag_counts = {
                row[0]: row[1]
                for row in conn.execute(
                    "SELECT tag, COUNT(*) FROM meetings WHERE archived = 0 GROUP BY tag"
                )
            }
            last_received = conn.execute(
                "SELECT received_at FROM meetings WHERE archived = 0 ORDER BY id DESC LIMIT 1"
            ).fetchone()
            return {
                "total": total,
                "completed": completed,
                "failed": failed,
                "in_progress": in_progress,
                "archived": archived,
                "total_bytes": total_bytes,
                "tag_counts": tag_counts,
                "last_received": last_received[0] if last_received else None,
            }


class TodoStore:
    def __init__(self, db_path: Path):
        self.db_path = db_path
        init_db(db_path)

    @contextmanager
    def _connect(self):
        conn = sqlite3.connect(self.db_path)
        conn.row_factory = sqlite3.Row
        try:
            yield conn
            conn.commit()
        finally:
            conn.close()

    def replace_for_meeting(self, meeting_id: int, texts):
        """Swaps out every to-do for this meeting for a fresh set extracted
        from its latest cleanup, so retagging or resubmitting a recording
        doesn't leave stale or duplicate items behind."""
        now = _now_iso()
        rows = []
        for item in texts:
            # Plain strings (legacy extraction) or dicts (structured tasks).
            if isinstance(item, str):
                item = {"text": item}
            rows.append((
                meeting_id, item["text"], now, now,
                item.get("due_utc") or None,
                item.get("type") or "todo",
                item.get("date_precision") or "none",
                item.get("confidence"),
                item.get("original_time_text") or None,
            ))
        with self._connect() as conn:
            # Keep completion state across a regeneration: otherwise
            # retagging or retrying a note would silently reopen tasks the
            # user already ticked off.
            done_texts = {
                r["text"].strip().lower()
                for r in conn.execute(
                    "SELECT text FROM todos WHERE meeting_id = ? AND done = 1", (meeting_id,))
            }
            conn.execute("DELETE FROM todos WHERE meeting_id = ?", (meeting_id,))
            conn.executemany(
                "INSERT INTO todos (meeting_id, text, done, created_at, updated_at, "
                "due_utc, type, date_precision, confidence, original_time_text) "
                "VALUES (?, ?, 0, ?, ?, ?, ?, ?, ?, ?)",
                rows,
            )
            if done_texts:
                conn.execute(
                    "UPDATE todos SET done = 1 WHERE meeting_id = ? AND lower(trim(text)) IN (%s)"
                    % ",".join("?" * len(done_texts)),
                    [meeting_id, *done_texts],
                )

    def list_all(self, done: bool = None):
        sql = (
            "SELECT todos.*, meetings.meeting_name, meetings.title AS meeting_title, "
            "meetings.tag, meetings.archived "
            "FROM todos JOIN meetings ON meetings.id = todos.meeting_id "
            "WHERE meetings.archived = 0"
        )
        params = []
        if done is not None:
            sql += " AND todos.done = ?"
            params.append(1 if done else 0)
        sql += " ORDER BY todos.id DESC"
        with self._connect() as conn:
            return [dict(r) for r in conn.execute(sql, params).fetchall()]

    def clear_apple_exported(self):
        with self._connect() as conn:
            conn.execute("UPDATE todos SET apple_exported_at = NULL")

    def mark_apple_exported(self, todo_ids):
        ids = list(todo_ids)
        if not ids:
            return
        now = _now_iso()
        placeholders = ",".join("?" * len(ids))
        with self._connect() as conn:
            conn.execute(
                f"UPDATE todos SET apple_exported_at = ? WHERE id IN ({placeholders})",
                [now, *ids],
            )

    def get(self, todo_id: int):
        with self._connect() as conn:
            row = conn.execute("SELECT * FROM todos WHERE id = ?", (todo_id,)).fetchone()
            return dict(row) if row else None

    def set_done(self, todo_id: int, done: bool):
        now = _now_iso()
        with self._connect() as conn:
            conn.execute(
                "UPDATE todos SET done = ?, updated_at = ? WHERE id = ?",
                (1 if done else 0, now, todo_id),
            )

    def set_text(self, todo_id: int, text: str):
        now = _now_iso()
        with self._connect() as conn:
            conn.execute(
                "UPDATE todos SET text = ?, updated_at = ? WHERE id = ?",
                (text, now, todo_id),
            )

    def counts(self):
        with self._connect() as conn:
            open_count = conn.execute(
                "SELECT COUNT(*) FROM todos JOIN meetings ON meetings.id = todos.meeting_id "
                "WHERE meetings.archived = 0 AND todos.done = 0"
            ).fetchone()[0]
            closed_count = conn.execute(
                "SELECT COUNT(*) FROM todos JOIN meetings ON meetings.id = todos.meeting_id "
                "WHERE meetings.archived = 0 AND todos.done = 1"
            ).fetchone()[0]
            return {"open": open_count, "closed": closed_count}
