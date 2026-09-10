#!/usr/bin/env python3
"""
AI Meeting Buddy - remote MCP server (Streamable HTTP + bearer auth).

Same 3 tools as server.py (stdio), but served over HTTP so it can be added
as a Custom Connector at claude.ai/settings/connectors. Put this behind the
same reverse proxy that already terminates HTTPS for the Flask app, on a
separate port or path.

Config (env vars, e.g. in mcp-server/.env):
    MEETING_BUDDY_URL           Base URL of the Flask app (unchanged from server.py)
    MEETING_BUDDY_READ_TOKEN    TODOS_READ_TOKEN or UPLOAD_TOKEN from server/.env
    MEETING_BUDDY_WRITE_TOKEN   UPLOAD_TOKEN from server/.env (optional)
    MCP_CONNECTOR_TOKEN         Bearer token clients must send to reach THIS
                                 server. Separate from the tokens above -
                                 generate with:
                                 python -c "import secrets; print(secrets.token_urlsafe(32))"
    MCP_HTTP_HOST               Default 0.0.0.0
    MCP_HTTP_PORT               Default 8788

Run:
    pip install -r requirements.txt
    python server_http.py
"""

import hmac
import os

import uvicorn
from dotenv import load_dotenv
from starlette.applications import Starlette
from starlette.responses import JSONResponse
from starlette.types import ASGIApp, Receive, Scope, Send

from server import mcp  # reuses the same tool definitions as the stdio server

load_dotenv(override=True)

CONNECTOR_TOKEN = os.environ.get("MCP_CONNECTOR_TOKEN", "")
HTTP_HOST = os.environ.get("MCP_HTTP_HOST", "0.0.0.0")
HTTP_PORT = int(os.environ.get("MCP_HTTP_PORT", "8788"))

if not CONNECTOR_TOKEN:
    raise RuntimeError(
        "MCP_CONNECTOR_TOKEN must be set - this is the bearer token claude.ai's "
        "connector will send. Generate one with: "
        "python -c \"import secrets; print(secrets.token_urlsafe(32))\""
    )


class BearerAuthMiddleware:
    """Rejects any request that doesn't present the exact connector token as
    a Bearer credential, before it ever reaches the MCP app."""

    def __init__(self, app: ASGIApp):
        self.app = app

    async def __call__(self, scope: Scope, receive: Receive, send: Send):
        if scope["type"] != "http":
            await self.app(scope, receive, send)
            return

        headers = dict(scope.get("headers", []))
        auth = headers.get(b"authorization", b"").decode("latin-1")
        supplied = auth[7:] if auth.lower().startswith("bearer ") else ""
        if not supplied or not hmac.compare_digest(supplied, CONNECTOR_TOKEN):
            response = JSONResponse({"error": "unauthorized"}, status_code=401)
            await response(scope, receive, send)
            return

        await self.app(scope, receive, send)


inner_app = mcp.streamable_http_app()
app = Starlette(routes=inner_app.routes, lifespan=inner_app.router.lifespan_context)
app.add_middleware(BearerAuthMiddleware)

if __name__ == "__main__":
    uvicorn.run(app, host=HTTP_HOST, port=HTTP_PORT)
