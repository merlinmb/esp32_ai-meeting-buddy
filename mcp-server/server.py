#!/usr/bin/env python3
"""
AI Meeting Buddy - MCP wrapper.

A thin stdio MCP server exposing the existing /api/todos and /api/stats
HTTP endpoints as tools, since Claude connectors can only talk MCP, not a
plain REST API.

Config (env vars, e.g. in the MCP client's config or a local .env):
    MEETING_BUDDY_URL          Base URL of the Flask app, e.g. https://notes.example.com
    MEETING_BUDDY_READ_TOKEN   TODOS_READ_TOKEN (or UPLOAD_TOKEN) from the server's .env
                                - enables list_todos and get_stats.
    MEETING_BUDDY_WRITE_TOKEN  UPLOAD_TOKEN from the server's .env (optional) -
                                enables mark_todo_done. Falls back to
                                MEETING_BUDDY_READ_TOKEN if that value is
                                itself the full UPLOAD_TOKEN.

Run:
    pip install -r requirements.txt
    python server.py
"""

import os

import requests
from dotenv import load_dotenv
from mcp.server.fastmcp import FastMCP
from mcp.server.transport_security import TransportSecuritySettings

load_dotenv(override=True)

BASE_URL = os.environ.get("MEETING_BUDDY_URL", "").rstrip("/")
READ_TOKEN = os.environ.get("MEETING_BUDDY_READ_TOKEN", "")
WRITE_TOKEN = os.environ.get("MEETING_BUDDY_WRITE_TOKEN", "") or READ_TOKEN

if not BASE_URL:
    raise RuntimeError("MEETING_BUDDY_URL must be set - the base URL of the AI Meeting Buddy server.")
if not READ_TOKEN:
    raise RuntimeError(
        "MEETING_BUDDY_READ_TOKEN must be set - use the server's TODOS_READ_TOKEN "
        "(read-only) or UPLOAD_TOKEN (full access) from its .env."
    )

# MCP_PUBLIC_HOST is the Host header clients will actually send (e.g. the
# domain behind the reverse proxy) - the SDK's DNS-rebinding protection
# rejects any Host/Origin not in this allowlist.
_public_host = os.environ.get("MCP_PUBLIC_HOST", "")
_allowed_hosts = ["127.0.0.1", "127.0.0.1:*", "localhost", "localhost:*"]
_allowed_origins = ["http://127.0.0.1:*", "http://localhost:*"]
if _public_host:
    _allowed_hosts.append(_public_host)
    _allowed_origins.append(f"https://{_public_host}")

mcp = FastMCP(
    "ai-meeting-buddy-todos",
    transport_security=TransportSecuritySettings(
        allowed_hosts=_allowed_hosts,
        allowed_origins=_allowed_origins,
    ),
)


def _get(path: str, token: str, params: dict = None) -> dict:
    resp = requests.get(
        f"{BASE_URL}{path}",
        headers={"X-Upload-Token": token},
        params=params or {},
        timeout=30,
    )
    resp.raise_for_status()
    return resp.json()


def _post(path: str, token: str, json_body: dict) -> dict:
    resp = requests.post(
        f"{BASE_URL}{path}",
        headers={"X-Upload-Token": token},
        json=json_body,
        timeout=30,
    )
    resp.raise_for_status()
    return resp.json()


@mcp.tool()
def list_todos(done: bool | None = None) -> dict:
    """List to-do items extracted from voice notes tagged Note, Idea, or Buy.
    Meetings are excluded. Pass done=True for completed items, done=False for
    open items, or omit to get both."""
    params = {}
    if done is not None:
        params["done"] = "1" if done else "0"
    return _get("/api/todos", READ_TOKEN, params)


@mcp.tool()
def mark_todo_done(todo_id: int, done: bool = True) -> dict:
    """Mark a to-do item done or not done, by its id (from list_todos)."""
    return _post(f"/api/todos/{todo_id}", WRITE_TOKEN, {"done": done})


@mcp.tool()
def get_stats() -> dict:
    """Overall AI Meeting Buddy stats: recording counts by status, to-do
    open/closed counts, and processing queue length. Useful for a daily
    summary report."""
    return _get("/api/stats", READ_TOKEN)


if __name__ == "__main__":
    mcp.run(transport="stdio")
