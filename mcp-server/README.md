# AI Meeting Buddy - MCP wrapper

Thin stdio MCP server exposing the existing to-do/stats HTTP API as tools,
since Claude connectors need MCP, not a plain REST API.

## Tools

- `list_todos(done: bool | None)` - to-do items from Note/Idea/Buy recordings
  (meetings excluded). `done=True`/`False` filters, omit for both.
- `mark_todo_done(todo_id: int, done: bool = True)` - check an item off (or
  back on). Requires `MEETING_BUDDY_WRITE_TOKEN`.
- `get_stats()` - recording counts, to-do open/closed counts, queue length.

## Setup

```
pip install -r requirements.txt
cp .env.example .env
```

Fill in `.env`:
- `MEETING_BUDDY_URL` - the server's public URL (e.g. `https://notes.yourdomain.com`)
- `MEETING_BUDDY_READ_TOKEN` - `TODOS_READ_TOKEN` from `server/.env` (or
  `UPLOAD_TOKEN` if you haven't set one)
- `MEETING_BUDDY_WRITE_TOKEN` - `UPLOAD_TOKEN` from `server/.env`, only
  needed for `mark_todo_done`. Leave blank for read-only.

## Registering with Claude Code / Claude Desktop

Add to the MCP config (stdio transport, launched as a local process):

```json
{
  "mcpServers": {
    "ai-meeting-buddy": {
      "command": "python",
      "args": ["e:/Development/AI/ai-meeting-buddy/mcp-server/server.py"],
      "env": {
        "MEETING_BUDDY_URL": "https://notes.yourdomain.com",
        "MEETING_BUDDY_READ_TOKEN": "...",
        "MEETING_BUDDY_WRITE_TOKEN": "..."
      }
    }
  }
}
```

(Values can also just live in `mcp-server/.env` since the server loads it
itself - the config's `env` block is only needed if you'd rather not keep a
`.env` file next to the script.)

## Registering as a claude.ai Custom Connector (remote/HTTP)

`server.py` (stdio) only works with a local Claude Code/Desktop process - it
has no URL. To add this as a Custom Connector at
claude.ai/settings/connectors, run `server_http.py` instead: it serves the
same 3 tools over Streamable HTTP with bearer-token auth, so it can sit
behind the reverse proxy already terminating HTTPS for the Flask app.

1. Fill in `.env`: `MEETING_BUDDY_URL`, `MEETING_BUDDY_READ_TOKEN`,
   optionally `MEETING_BUDDY_WRITE_TOKEN`, and `MCP_CONNECTOR_TOKEN`
   (generate with `python -c "import secrets; print(secrets.token_urlsafe(32))"`
   - this is a *different* token from the read/write ones above; it's what
   protects this server itself, not the Flask app it calls). Also set
   `MCP_PUBLIC_HOST` to the domain clients will hit (e.g.
   `meetings.mgbeets.com`) - the MCP SDK's DNS-rebinding protection rejects
   any request whose `Host` header isn't allowlisted.
2. Run it (`python server_http.py`, or via the `meeting-buddy-mcp` service in
   `server/docker-compose.yml`) - it listens on `MCP_HTTP_PORT` (default 8788).
3. Point your reverse proxy at it, e.g. `https://notes.yourdomain.com/mcp` ->
   `http://<host>:8788/mcp` (same pattern as the existing proxy rule for the
   dashboard on 8787, just a different upstream port/path). It must be
   reachable over HTTPS for claude.ai to accept it.
4. In claude.ai -> Settings -> Connectors -> Add custom connector:
   - **URL**: `https://notes.yourdomain.com/mcp` (your proxied HTTPS endpoint)
   - **Authorization**: Bearer token = your `MCP_CONNECTOR_TOKEN` value

Anyone with that bearer token gets read access to your todos/stats (and
write access too, if `MEETING_BUDDY_WRITE_TOKEN` is set) - treat it like a
password.
