#!/usr/bin/env bash
# Deploys the AI Meeting Buddy receiver to savage.local using a Docker
# "SSH context" - your local Docker CLI talks to the remote Docker daemon
# over SSH and streams the build context to it directly. No registry, no
# manual file copying, no separate deploy tooling required.
#
# Prerequisites on savage.local:
#   - Docker (and the Compose plugin) installed and running
#   - your SSH key authorized for the account you connect as
#   - that account can run `docker` (in the "docker" group, or root)
#
# Prerequisites here:
#   - Docker Desktop / Docker CLI with the compose plugin
#   - `cp .env.example .env` and fill in ANTHROPIC_API_KEY at minimum
#
# Usage:
#   ./deploy.sh setup              # one-time: register the remote context
#   ./deploy.sh deploy              # fast path: rebuild changed layers + (re)start
#   ./deploy.sh deploy --full       # full rebuild: --no-cache, then recreate container
#   ./deploy.sh logs                # tail the remote container's logs
#   ./deploy.sh down                # stop the remote container
#   ./deploy.sh pull-transcripts    # copy transcripts to ./transcripts_from_savage
#
# Override the host/user without editing this file:
#   SAVAGE_HOST=savage.local SAVAGE_USER=pi ./deploy.sh deploy

set -euo pipefail
cd "$(dirname "$0")"

REMOTE_HOST="${SAVAGE_HOST:-savage.local}"
REMOTE_USER="${SAVAGE_USER:-$(whoami)}"
CONTEXT_NAME="savage-local"
COMPOSE_FILE="docker-compose.yml"

require_context() {
  if ! docker context inspect "$CONTEXT_NAME" >/dev/null 2>&1; then
    echo "Docker context '$CONTEXT_NAME' doesn't exist yet - run '$0 setup' first." >&2
    exit 1
  fi
}

cmd="${1:-}"

case "$cmd" in
  setup)
    if docker context inspect "$CONTEXT_NAME" >/dev/null 2>&1; then
      echo "Context '$CONTEXT_NAME' already exists (pointing at whatever host it was created with)."
      echo "Remove it first with 'docker context rm $CONTEXT_NAME' if you need to change host/user."
    else
      docker context create "$CONTEXT_NAME" \
        --docker "host=ssh://${REMOTE_USER}@${REMOTE_HOST}"
      echo "Created docker context '$CONTEXT_NAME' -> ssh://${REMOTE_USER}@${REMOTE_HOST}"
    fi
    echo "Verifying it can reach the remote daemon..."
    docker --context "$CONTEXT_NAME" info >/dev/null && echo "OK - savage.local is reachable and Docker is running there."
    ;;

  deploy)
    require_context
    if [ ! -f .env ]; then
      echo "No .env found here - copy .env.example to .env and fill in ANTHROPIC_API_KEY first." >&2
      exit 1
    fi
    if [ "${2:-}" = "--full" ]; then
      echo "Full deploy: rebuilding all layers from scratch and recreating the container..."
      docker --context "$CONTEXT_NAME" compose -f "$COMPOSE_FILE" build --no-cache
      docker --context "$CONTEXT_NAME" compose -f "$COMPOSE_FILE" up -d --force-recreate
    else
      docker --context "$CONTEXT_NAME" compose -f "$COMPOSE_FILE" up -d --build
    fi
    echo
    echo "Deployed. Receiver should be reachable at http://${REMOTE_HOST}:8787/health"
    echo "Point the firmware's UPLOAD_SERVER_URL in config.h at http://${REMOTE_HOST}:8787/upload"
    ;;

  logs)
    require_context
    docker --context "$CONTEXT_NAME" compose -f "$COMPOSE_FILE" logs -f
    ;;

  down)
    require_context
    docker --context "$CONTEXT_NAME" compose -f "$COMPOSE_FILE" down
    ;;

  restart)
    require_context
    docker --context "$CONTEXT_NAME" compose -f "$COMPOSE_FILE" restart
    ;;

  pull-transcripts)
    require_context
    mkdir -p ./transcripts_from_savage
    docker --context "$CONTEXT_NAME" cp ai-meeting-buddy-receiver:/app/transcripts/. ./transcripts_from_savage/
    echo "Copied transcripts to ./transcripts_from_savage"
    ;;

  *)
    echo "Usage: $0 {setup|deploy|logs|down|restart|pull-transcripts}"
    exit 1
    ;;
esac
