"""Offline checks for the generated Apple Shortcut plist."""

import plistlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from apple_shortcut import SOURCE_URL, build_shortcut  # noqa: E402


def main():
    token = "test-read-token"
    workflow = plistlib.loads(build_shortcut(token))
    actions = workflow["WFWorkflowActions"]
    identifiers = [action["WFWorkflowActionIdentifier"] for action in actions]

    assert workflow["WFWorkflowName"] == "AI Meeting Buddy Sync"
    assert SOURCE_URL == "https://meetings.mgbeets.com/api/apple-shortcut/data"
    assert "is.workflow.actions.downloadurl" in identifiers
    assert "is.workflow.actions.detect.dictionary" in identifiers
    assert "is.workflow.actions.detect.date" in identifiers
    assert "is.workflow.actions.createnote" in identifiers
    assert "is.workflow.actions.addnewreminder" in identifiers
    assert identifiers.count("is.workflow.actions.repeat.each") == 4

    serialized = plistlib.dumps(workflow, fmt=plistlib.FMT_XML).decode()
    assert token in serialized
    assert SOURCE_URL in serialized
    assert "WFAlertCustomTime" in serialized

    try:
        build_shortcut("")
    except ValueError:
        pass
    else:
        raise AssertionError("Generating a Shortcut without a token must fail")


if __name__ == "__main__":
    main()
