"""Generate the downloadable Apple Notes and Reminders Shortcut."""

import plistlib
import uuid


SOURCE_URL = "https://meetings.mgbeets.com/api/apple-shortcut/data"
SHORTCUT_NAME = "AI Meeting Buddy Sync"


def _uuid() -> str:
    return str(uuid.uuid4()).upper()


def _action(identifier: str, **parameters) -> dict:
    parameters.setdefault("UUID", _uuid())
    return {
        "WFWorkflowActionIdentifier": identifier,
        "WFWorkflowActionParameters": parameters,
    }


def _action_output(action_id: str, output_name: str) -> dict:
    return {
        "Value": {
            "Type": "ActionOutput",
            "OutputUUID": action_id,
            "OutputName": output_name,
        },
        "WFSerializationType": "WFTextTokenAttachment",
    }


def _variable(name: str) -> dict:
    return {
        "Value": {"Type": "Variable", "VariableName": name},
        "WFSerializationType": "WFTextTokenAttachment",
    }


def _text(parts) -> dict:
    text = ""
    attachments = {}
    offset = 0
    for part in parts:
        if isinstance(part, str):
            text += part
            offset += len(part)
            continue
        text += "\ufffc"
        attachments[f"{{{offset}, 1}}"] = part["Value"]
        offset += 1
    return {
        "Value": {"string": text, "attachmentsByRange": attachments},
        "WFSerializationType": "WFTextTokenString",
    }


def _field(actions: list, key: str, variable_name: str) -> str:
    lookup_id = _uuid()
    actions.append(_action(
        "is.workflow.actions.getvalueforkey",
        UUID=lookup_id,
        WFInput=_variable("Repeat Item"),
        WFDictionaryKey=key,
    ))
    actions.append(_action(
        "is.workflow.actions.setvariable",
        WFVariableName=variable_name,
        WFInput=_action_output(lookup_id, "Dictionary Value"),
    ))
    return variable_name


def _filter_action(action_id: str, property_name: str, marker_variable: str) -> dict:
    return _action(
        action_id,
        WFContentItemLimitEnabled=False,
        WFContentItemFilter={
            "Value": {
                "WFActionParameterFilterPrefix": 1,
                "WFContentPredicateBoundedDate": False,
                "WFActionParameterFilterTemplates": [{
                    "Property": property_name,
                    "Operator": 4,
                    "Values": {"String": _text([_variable(marker_variable)])},
                    "VariableOverrides": {},
                }],
            },
            "WFSerializationType": "WFContentPredicateTableTemplate",
        },
    )


def _count_action(actions: list, source_id: str, output_name: str) -> str:
    count_id = _uuid()
    actions.append(_action(
        "is.workflow.actions.count",
        UUID=count_id,
        Input=_action_output(source_id, output_name),
    ))
    return count_id


def _if_count_is_zero(actions: list, count_id: str, group_id: str) -> None:
    actions.append(_action(
        "is.workflow.actions.conditional",
        GroupingIdentifier=group_id,
        WFControlFlowMode=0,
        WFInput=_action_output(count_id, "Count"),
        WFCondition=0,
        WFNumberValue=0,
    ))


def _end_if(actions: list, group_id: str) -> None:
    actions.append(_action(
        "is.workflow.actions.conditional",
        GroupingIdentifier=group_id,
        WFControlFlowMode=2,
    ))


def _end_repeat(actions: list, group_id: str) -> None:
    actions.append(_action(
        "is.workflow.actions.repeat.each",
        GroupingIdentifier=group_id,
        WFControlFlowMode=2,
    ))


def _notes_actions(actions: list, notes_output_id: str) -> None:
    repeat_id = _uuid()
    actions.append(_action(
        "is.workflow.actions.repeat.each",
        GroupingIdentifier=repeat_id,
        WFControlFlowMode=0,
        WFInput=_action_output(notes_output_id, "Dictionary Value"),
    ))
    _field(actions, "id", "Note ID")
    _field(actions, "name", "Note Name")
    _field(actions, "content", "Note Content")

    marker_id = _uuid()
    actions.append(_action(
        "is.workflow.actions.gettext",
        UUID=marker_id,
        WFTextActionText=_text([
            "[Meeting Buddy #", _variable("Note ID"), "]",
        ]),
    ))
    actions.append(_action(
        "is.workflow.actions.setvariable",
        WFVariableName="Note Marker",
        WFInput=_action_output(marker_id, "Text"),
    ))

    find_id = _uuid()
    filter_action = _filter_action(
        "is.workflow.actions.filter.notes", "Name", "Note Marker"
    )
    filter_action["WFWorkflowActionParameters"]["UUID"] = find_id
    actions.append(filter_action)
    count_id = _count_action(actions, find_id, "Notes")
    group_id = _uuid()
    _if_count_is_zero(actions, count_id, group_id)
    body_id = _uuid()
    actions.append(_action(
        "is.workflow.actions.gettext",
        UUID=body_id,
        WFTextActionText=_text([
            "Meeting Buddy - ", _variable("Note Name"), " ",
            _variable("Note Marker"), "\n\n", _variable("Note Content"),
        ]),
    ))
    actions.append(_action(
        "is.workflow.actions.createnote",
        WFCreateNoteInput=_action_output(body_id, "Text"),
        WFShowWhenRun=False,
    ))
    _end_if(actions, group_id)
    _end_repeat(actions, repeat_id)


def _reminder_action(
    title_variable: str,
    meeting_variable: str,
    due_variable: str | None = None,
) -> dict:
    params = {
        "WFCalendarItemTitle": _text([_variable(title_variable)]),
        "WFCalendarItemNotes": _text([
            "Imported from AI Meeting Buddy. Source meeting: ",
            _variable(meeting_variable),
        ]),
    }
    if due_variable:
        params.update({
            "WFAlertEnabled": "Alert",
            "WFAlertCondition": "At Time",
            "WFAlertCustomTime": _text([_variable(due_variable)]),
        })
    return _action("is.workflow.actions.addnewreminder", **params)


def _todos_actions(actions: list, todos_output_id: str) -> None:
    repeat_id = _uuid()
    actions.append(_action(
        "is.workflow.actions.repeat.each",
        GroupingIdentifier=repeat_id,
        WFControlFlowMode=0,
        WFInput=_action_output(todos_output_id, "Dictionary Value"),
    ))
    _field(actions, "id", "Task ID")
    _field(actions, "text", "Task Text")
    _field(actions, "meeting_name", "Meeting Name")
    _field(actions, "due_utc", "Task Due UTC")

    marker_id = _uuid()
    actions.append(_action(
        "is.workflow.actions.gettext",
        UUID=marker_id,
        WFTextActionText=_text([
            _variable("Task Text"), " [Meeting Buddy #",
            _variable("Task ID"), "]",
        ]),
    ))
    actions.append(_action(
        "is.workflow.actions.setvariable",
        WFVariableName="Task Reminder Title",
        WFInput=_action_output(marker_id, "Text"),
    ))

    find_id = _uuid()
    filter_action = _filter_action(
        "is.workflow.actions.filter.reminders", "Title", "Task Reminder Title"
    )
    filter_action["WFWorkflowActionParameters"]["UUID"] = find_id
    actions.append(filter_action)
    count_id = _count_action(actions, find_id, "Reminders")
    group_id = _uuid()
    _if_count_is_zero(actions, count_id, group_id)

    due_group_id = _uuid()
    actions.append(_action(
        "is.workflow.actions.conditional",
        GroupingIdentifier=due_group_id,
        WFControlFlowMode=0,
        WFInput=_variable("Task Due UTC"),
        WFCondition=4,
    ))
    due_date_id = _uuid()
    actions.append(_action(
        "is.workflow.actions.detect.date",
        UUID=due_date_id,
        WFInput=_variable("Task Due UTC"),
    ))
    actions.append(_action(
        "is.workflow.actions.setvariable",
        WFVariableName="Task Due Date",
        WFInput=_action_output(due_date_id, "Dates"),
    ))
    actions.append(_reminder_action(
        "Task Reminder Title", "Meeting Name", "Task Due Date"
    ))
    actions.append(_action(
        "is.workflow.actions.conditional",
        GroupingIdentifier=due_group_id,
        WFControlFlowMode=1,
    ))
    actions.append(_reminder_action("Task Reminder Title", "Meeting Name"))
    _end_if(actions, due_group_id)
    _end_if(actions, group_id)
    _end_repeat(actions, repeat_id)


def build_shortcut(read_token: str) -> bytes:
    """Return an unsigned Apple Shortcut plist with the read token embedded."""
    if not read_token:
        raise ValueError("A read-only token is required to generate this Shortcut.")

    url_id = _uuid()
    request_id = _uuid()
    dictionary_id = _uuid()
    notes_id = _uuid()
    todos_id = _uuid()
    actions = [
        _action(
            "is.workflow.actions.gettext",
            UUID=url_id,
            WFTextActionText=SOURCE_URL,
        ),
        _action(
            "is.workflow.actions.downloadurl",
            UUID=request_id,
            WFURL=_action_output(url_id, "Text"),
            WFHTTPMethod="GET",
            WFHTTPHeaders={
                "Value": {
                    "WFDictionaryFieldValueItems": [{
                        "WFItemType": 0,
                        "WFKey": {
                            "Value": {"string": "X-Upload-Token"},
                            "WFSerializationType": "WFTextTokenString",
                        },
                        "WFValue": {
                            "Value": {"string": read_token},
                            "WFSerializationType": "WFTextTokenString",
                        },
                    }],
                },
                "WFSerializationType": "WFDictionaryFieldValue",
            },
        ),
        _action(
            "is.workflow.actions.detect.dictionary",
            UUID=dictionary_id,
            WFInput=_action_output(request_id, "Contents of URL"),
        ),
        _action(
            "is.workflow.actions.getvalueforkey",
            UUID=notes_id,
            WFInput=_action_output(dictionary_id, "Dictionary"),
            WFDictionaryKey="notes",
        ),
    ]
    _notes_actions(actions, notes_id)
    actions.append(_action(
        "is.workflow.actions.getvalueforkey",
        UUID=todos_id,
        WFInput=_action_output(dictionary_id, "Dictionary"),
        WFDictionaryKey="todos",
    ))
    _todos_actions(actions, todos_id)

    workflow = {
        "WFWorkflowName": SHORTCUT_NAME,
        "WFWorkflowActions": actions,
        "WFWorkflowClientVersion": "2600",
        "WFWorkflowMinimumClientVersion": 900,
        "WFWorkflowMinimumClientVersionString": "900",
        "WFWorkflowTypes": [],
        "WFWorkflowHasShortcutInputVariables": False,
        "WFWorkflowHasOutputAction": False,
        "WFWorkflowInputContentItemClasses": [],
        "WFWorkflowIcon": {
            "WFWorkflowIconStartColor": 4251333119,
            "WFWorkflowIconGlyphNumber": 59507,
        },
        "WFWorkflowImportQuestions": [],
    }
    return plistlib.dumps(workflow, fmt=plistlib.FMT_XML, sort_keys=False)
