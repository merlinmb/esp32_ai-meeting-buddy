"""Offline tests for brain.py and the db migrations (no LLM, no network).
Run from server/:  python test/test_brain.py
"""
import json
import os
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
os.environ["LOCAL_TIMEZONE"] = "Europe/Berlin"

import brain  # noqa: E402
from db import MeetingStore, TodoStore  # noqa: E402


def main():
    tmp = Path(tempfile.mkdtemp()) / "m.db"
    store = MeetingStore(tmp)
    todos = TodoStore(tmp)
    bs = brain.BrainStore(tmp)

    # Re-opening an existing DB must be a no-op for migrations.
    MeetingStore(tmp)

    captured = datetime(2026, 5, 4, 8, 0, tzinfo=timezone.utc)  # Monday
    tz = brain.local_tz()

    due, prec = brain.resolve_due("1999-01-01", "15:30", "tomorrow at 3:30pm", captured, tz)
    assert prec == "time" and due.startswith("2026-05-05T13:30"), (due, prec)  # CEST = UTC+2
    due, prec = brain.resolve_due("2026-05-08", "", "on Friday", captured, tz)
    assert prec == "day" and due.startswith("2026-05-07T22:00"), (due, prec)
    assert brain.resolve_due("garbage", "", "sometime", captured, tz) == (None, "none")

    assert brain.extract_json('noise ```json\n{"a": {"b": "}"}}\n``` tail') == {"a": {"b": "}"}}
    assert brain.extract_json("no json") is None

    raw = {"title": "Dentist", "summary": "Book dentist", "importance": 9,
           "tasks": [
               {"title": "Dentist visit", "type": "event", "date": "2026-05-05", "time": "15:30",
                "original_time_text": "tomorrow at 3:30", "confidence": 90},
               {"title": "Call mum", "type": "event", "date": "", "time": "", "confidence": 80},
               {"title": "Vague", "type": "todo", "date": "2026-05-06", "confidence": 10}]}
    mem = brain.normalize_memory(raw, captured, tz)
    assert mem["importance"] == 3
    assert [t["type"] for t in mem["tasks"]] == ["event", "todo", "todo"]
    assert mem["tasks"][2]["due_utc"] is None  # low confidence -> no date

    class Fake:
        def __call__(self, prompt, json_mode=False):
            return json.dumps(raw)

    md = Path(tmp.parent) / "n.md"
    md.write_text("# n\n\nbody about dentist", encoding="utf-8")
    mid = store.create("note_001_20260504080000", "x.wav", 10)
    store.save_result(mid, str(md))
    store.set_tag(mid, "Note")
    meeting = store.get(mid)
    assert brain.capture_time_utc(meeting) == captured.replace(hour=8)

    memory = brain.extract_memory(Fake(), "body", meeting)
    bs.save_memory(mid, memory)
    todos.replace_for_meeting(mid, memory["tasks"])
    assert bs.get_memory(mid)["title"] == "Dentist"
    rows = todos.list_all()
    assert len(rows) == 3 and any(r["type"] == "event" and r["due_utc"] for r in rows)

    # Completed state survives regeneration.
    todos.set_done(rows[0]["id"], True)
    todos.replace_for_meeting(mid, memory["tasks"])
    assert sum(1 for r in todos.list_all() if r["done"]) == 1

    # Plain strings still work (legacy path).
    todos.replace_for_meeting(mid, ["a", "b"])
    assert len(todos.list_all()) == 2
    todos.replace_for_meeting(mid, memory["tasks"])

    agenda = brain.agenda(bs, datetime(2026, 5, 5).date(), 1)
    assert [a["text"] for a in agenda] == ["Dentist visit"] and agenda[0]["local_time"] == "15:30", agenda
    b = brain.build_briefing(bs, "daily", datetime(2026, 5, 5, 6, 0, tzinfo=timezone.utc))
    assert "Dentist visit" in b["text"], b

    assert bs.replace_moments(mid, [5000, 1000, 1000, -3]) == [1000, 5000]
    assert bs.get_moments(mid) == [1000, 5000]

    bs.save_embedding(mid, "m", [1.0, 0.0])
    assert bs.all_embeddings("m")[mid] == [1.0, 0.0]

    mems = bs.list_memories()
    assert brain.retrieve("when is my dentist appointment", mems)[0]["id"] == mid
    assert brain.retrieve("quantum physics", mems) == []

    open_tasks, done_tasks = todos.list_all(done=False), todos.list_all(done=True)

    def cmd(prompt, json_mode=False):
        return json.dumps({"action": "complete", "task_id": open_tasks[0]["id"]})
    action, task = brain.apply_task_command(cmd, "dentist done", open_tasks, done_tasks)
    assert action == "complete" and task["id"] == open_tasks[0]["id"]
    bogus = lambda p, json_mode=False: json.dumps({"action": "complete", "task_id": 99999})  # noqa: E731
    assert brain.apply_task_command(bogus, "x", open_tasks, done_tasks) == ("none", None)

    print("OK")


if __name__ == "__main__":
    main()
