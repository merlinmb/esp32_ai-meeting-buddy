# Complete Guide: Generating and Hosting iOS Shortcuts on a Server

This guide explains how to dynamically construct, host, and serve Apple Shortcuts (`.shortcut` files) from a web server. It includes the architecture, XML plist structure, server configuration, and workflow for making authenticated API requests and iterating over dictionary items to create Reminders on iOS.

---

## 1. High-Level Architecture Overview

1. **User Request:** A user clicks a link or button on your website/app on an iOS device using Safari.
2. **Server Processing:** The server dynamically injects user-specific parameters (such as an API token or endpoint URL) into a pre-configured `.shortcut` template (which is an XML Property List file).
3. **HTTP Response:** The server sends the `.shortcut` file with the MIME type `application/x-apple-shortcut`.
4. **iOS Import:** iOS automatically intercept the file and opens the native Shortcuts import screen, allowing the user to add it directly to their device.

---

## 2. Dynamic Dynamic XML Plist Structure

Apple Shortcuts are formatted as Apple Property Lists (plist). Below is an XML template demonstrating the core actions needed for your workflow:
- Setting an **Auth Token** variable
- Fetching data from an **API Endpoint** with dynamic headers
- **Iterating** over the JSON array
- Adding items to **iOS Reminders**

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>WFWorkflowActions</key>
	<array>
		<!-- ACTION 1: Define API Token Variable -->
		<dict>
			<key>WFWorkflowActionIdentifier</key>
			<string>is.workflow.actions.gettext</string>
			<key>WFWorkflowActionParameters</key>
			<dict>
				<key>WFTextActionText</key>
				<string>{{AUTH_TOKEN}}</string>
			</dict>
		</dict>
		<dict>
			<key>WFWorkflowActionIdentifier</key>
			<string>is.workflow.actions.setvariable</string>
			<key>WFWorkflowActionParameters</key>
			<dict>
				<key>WFVariableName</key>
				<string>AuthToken</string>
			</dict>
		</dict>

		<!-- ACTION 2: Get Contents of URL (HTTP GET Request) -->
		<dict>
			<key>WFWorkflowActionIdentifier</key>
			<string>is.workflow.actions.downloadurl</string>
			<key>WFWorkflowActionParameters</key>
			<dict>
				<key>WFURL</key>
				<string>{{API_URL}}</string>
				<key>WFHTTPHeaders</key>
				<dict>
					<key>Authorization</key>
					<string>Bearer {{AUTH_TOKEN}}</string>
					<key>Accept</key>
					<string>application/json</string>
				</dict>
				<key>WFHTTPMethod</key>
				<string>GET</string>
			</dict>
		</dict>

		<!-- ACTION 3: Parse Response JSON into Dictionary -->
		<dict>
			<key>WFWorkflowActionIdentifier</key>
			<string>is.workflow.actions.getvalueforkey</string>
			<key>WFWorkflowActionParameters</key>
			<dict>
				<key>WFGetDictionaryValueType</key>
				<string>Value</string>
				<key>WFDictionaryKey</key>
				<string>items</string>
			</dict>
		</dict>

		<!-- ACTION 4: Repeat With Each Item in Dictionary Loop -->
		<dict>
			<key>WFWorkflowActionIdentifier</key>
			<string>is.workflow.actions.repeat.each</string>
			<key>WFWorkflowActionParameters</key>
			<dict>
				<key>GroupingIdentifier</key>
				<string>REPEAT_LOOP_GROUP_ID</string>
				<key>WFControlFlowMode</key>
				<integer>0</integer>
			</dict>
		</dict>

		<!-- ACTION 5: Add New Item to Reminders -->
		<dict>
			<key>WFWorkflowActionIdentifier</key>
			<string>is.workflow.actions.addnewreminder</string>
			<key>WFWorkflowActionParameters</key>
			<dict>
				<key>WFCalendarItemTitle</key>
				<dict>
					<key>Value</key>
					<dict>
						<key>attachmentsByRange</key>
						<dict>
							<key>{0, 1}</key>
							<dict>
								<key>Type</key>
								<string>Repeat Item</string>
								<key>Aggrandizements</key>
								<array>
									<dict>
										<key>Type</key>
										<string>WFDictionaryValueVariableAggrandizement</string>
										<key>DictionaryKey</key>
										<string>title</string>
									</dict>
								</array>
							</dict>
						</dict>
						<key>string</key>
						<string>￼</string>
					</dict>
					<key>WFSerializationType</key>
					<string>WFTextTokenString</string>
				</dict>
			</dict>
		</dict>

		<!-- ACTION 6: End Repeat Loop -->
		<dict>
			<key>WFWorkflowActionIdentifier</key>
			<string>is.workflow.actions.repeat.each</string>
			<key>WFWorkflowActionParameters</key>
			<dict>
				<key>GroupingIdentifier</key>
				<string>REPEAT_LOOP_GROUP_ID</string>
				<key>WFControlFlowMode</key>
				<integer>2</integer>
			</dict>
		</dict>
	</array>
	<key>WFWorkflowMinimumClientVersion</key>
	<integer>900</integer>
	<key>WFWorkflowClientVersion</key>
	<string>1200</string>
</dict>
</plist>
```

---

## 3. Server Implementation Example (Python / Flask)

Below is an example Python server script that loads a template shortcut, dynamically inserts a per-user auth token, and serves the generated file.

```python
from flask import Flask, send_file, request, Response
import plistlib
import io

app = Flask(__name__)

# Template path
TEMPLATE_FILE = "template.shortcut"

@app.route('/download-shortcut')
def download_shortcut():
    # 1. Get dynamic user details (e.g., from session or auth query param)
    user_token = request.args.get('token', 'DEFAULT_BEARER_TOKEN')
    api_endpoint = "https://api.example.com/v1/tasks"

    # 2. Read base XML plist template
    with open(TEMPLATE_FILE, 'rb') as f:
        plist_data = plistlib.load(f)

    # 3. Dynamically inject values into action parameters
    for action in plist_data.get('WFWorkflowActions', []):
        action_id = action.get('WFWorkflowActionIdentifier')
        params = action.get('WFWorkflowActionParameters', {})

        # Inject Auth Token
        if action_id == 'is.workflow.actions.gettext':
            if params.get('WFTextActionText') == '{{AUTH_TOKEN}}':
                params['WFTextActionText'] = user_token

        # Inject Target URL & Headers
        if action_id == 'is.workflow.actions.downloadurl':
            if params.get('WFURL') == '{{API_URL}}':
                params['WFURL'] = api_endpoint
                params['WFHTTPHeaders']['Authorization'] = f"Bearer {user_token}"

    # 4. Serialize back to binary plist or XML format
    out_stream = io.BytesIO()
    plistlib.dump(plist_data, out_stream, fmt=plistlib.FMT_XML)
    out_stream.seek(0)

    # 5. Serve with proper headers for iOS recognition
    return Response(
        out_stream.getvalue(),
        mimetype='application/x-apple-shortcut',
        headers={
            "Content-Disposition": "attachment; filename=SyncReminders.shortcut"
        }
    )

if __name__ == '__main__':
    app.run(port=5000)
```

---

## 4. Key Server Configuration Requirements

To ensure smooth download and execution on iOS:

### HTTP Headers
The server response **must** include the correct MIME type header:
* `Content-Type: application/x-apple-shortcut`
* `Content-Disposition: attachment; filename="YourShortcutName.shortcut"`

### Safari Browser Requirement
The link must be opened inside **Safari** on iOS. Embedded browsers inside third-party apps (like Slack, Twitter, or webviews) might block direct installation of `.shortcut` files.

---

## 5. Alternative Approach: Native Import Questions

If server-side dynamic generation is not required, you can build a static `.shortcut` file on an iOS device and configure **Import Questions**:

1. Open the shortcut in the **Shortcuts app** on iOS or macOS.
2. Tap the **Settings** icon -> **Details** -> **Import Questions**.
3. Add a question for **API Token** and map it to your Text or Variable block.
4. Export the `.shortcut` file or share via an **iCloud Link**.
5. Host the static file on your web server.
6. When users import the file, iOS prompts them to enter their credentials directly.