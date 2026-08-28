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
#   .\deploy.ps1 setup              # one-time: register the remote context
#   .\deploy.ps1 deploy              # build + (re)start on savage.local
#   .\deploy.ps1 logs                # tail the remote container's logs
#   .\deploy.ps1 down                # stop the remote container
#   .\deploy.ps1 pull-transcripts    # copy transcripts to .\transcripts_from_savage
#
# Override the host/user without editing this file:
#   $env:SAVAGE_HOST="savage.local"; $env:SAVAGE_USER="pi"; .\deploy.ps1 deploy

param(
    [Parameter(Position = 0)]
    [string]$Command
)

$ErrorActionPreference = "Stop"
Set-Location -Path $PSScriptRoot

$RemoteHost = if ($env:SAVAGE_HOST) { $env:SAVAGE_HOST } else { "savage.local" }
$RemoteUser = if ($env:SAVAGE_USER) { $env:SAVAGE_USER } else { $env:USERNAME }
$ContextName = "savage-local"
$ComposeFile = "docker-compose.yml"

function Test-DockerContext($Name) {
    try {
        docker context inspect $Name 2>$null 1>$null
        return $LASTEXITCODE -eq 0
    }
    catch {
        return $false
    }
}

function Require-Context {
    if (-not (Test-DockerContext $ContextName)) {
        Write-Error "Docker context '$ContextName' doesn't exist yet - run '.\deploy.ps1 setup' first."
        exit 1
    }
}

switch ($Command) {
    "setup" {
        if (Test-DockerContext $ContextName) {
            Write-Host "Context '$ContextName' already exists (pointing at whatever host it was created with)."
            Write-Host "Remove it first with 'docker context rm $ContextName' if you need to change host/user."
        }
        else {
            docker context create $ContextName --docker "host=ssh://${RemoteUser}@${RemoteHost}"
            Write-Host "Created docker context '$ContextName' -> ssh://${RemoteUser}@${RemoteHost}"
        }
        Write-Host "Verifying it can reach the remote daemon..."
        try {
            docker --context $ContextName info 2>$null 1>$null
        }
        catch {}
        if ($LASTEXITCODE -eq 0) {
            Write-Host "OK - savage.local is reachable and Docker is running there."
        }
        else {
            Write-Error "Could not reach the remote Docker daemon via '$ContextName'."
            exit 1
        }
    }

    "deploy" {
        Require-Context
        if (-not (Test-Path ".env")) {
            Write-Error "No .env found here - copy .env.example to .env and fill in ANTHROPIC_API_KEY first."
            exit 1
        }
        docker --context $ContextName compose -f $ComposeFile up -d --build
        Write-Host ""
        Write-Host "Deployed. Receiver should be reachable at http://${RemoteHost}:8787/health"
        Write-Host "Point the firmware's UPLOAD_SERVER_URL in config.h at http://${RemoteHost}:8787/upload"
    }

    "logs" {
        Require-Context
        docker --context $ContextName compose -f $ComposeFile logs -f
    }

    "down" {
        Require-Context
        docker --context $ContextName compose -f $ComposeFile down
    }

    "restart" {
        Require-Context
        docker --context $ContextName compose -f $ComposeFile restart
    }

    "pull-transcripts" {
        Require-Context
        New-Item -ItemType Directory -Force -Path ".\transcripts_from_savage" | Out-Null
        docker --context $ContextName cp ai-meeting-buddy-receiver:/app/transcripts/. .\transcripts_from_savage\
        Write-Host "Copied transcripts to .\transcripts_from_savage"
    }

    default {
        Write-Host "Usage: .\deploy.ps1 {setup|deploy|logs|down|restart|pull-transcripts}"
        exit 1
    }
}
