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


def init_db(db_path: Path):
    db_path.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(db_path)
    conn.execute(_SCHEMA)
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
    conn.commit()
    conn.close()


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
