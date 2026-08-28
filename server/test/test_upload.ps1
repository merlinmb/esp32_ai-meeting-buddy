#!/usr/bin/env pwsh
<#
Test script: uploads test01_20s.wav to the running server's /upload endpoint.

If -ServerUrl is not given, it's built from UPLOAD_PORT in ../.env (default
port 8787) against localhost.

Run:
    ./test/test_upload.ps1 [-ServerUrl http://myhost:8787]
#>

param(
    [string]$ServerUrl
)

if (-not $ServerUrl) {
    $envPath = Join-Path $PSScriptRoot "..\.env"
    $port = "8787"
    if (Test-Path $envPath) {
        $match = Select-String -Path $envPath -Pattern '^\s*UPLOAD_PORT\s*=\s*(\S+)' | Select-Object -First 1
        if ($match) {
            $port = $match.Matches[0].Groups[1].Value
        }
    }
    $ServerUrl = "http://localhost:$port"
}

$WavPath = Join-Path $PSScriptRoot "MEETING_20260829_210333.wav"
$UploadUrl = "$($ServerUrl.TrimEnd('/'))/upload"

if (-not (Test-Path $WavPath)) {
    Write-Error "Test file not found: $WavPath"
    exit 1
}

Write-Host "Uploading $(Split-Path $WavPath -Leaf) to $UploadUrl ..."

$fileName = Split-Path $WavPath -Leaf
$boundary = [System.Guid]::NewGuid().ToString()
$fileBytes = [System.IO.File]::ReadAllBytes($WavPath)

$encoding = [System.Text.Encoding]::GetEncoding("ISO-8859-1")
$bodyLines = (
    "--$boundary",
    "Content-Disposition: form-data; name=`"audio`"; filename=`"$fileName`"",
    "Content-Type: audio/wav",
    "",
    $encoding.GetString($fileBytes),
    "--$boundary--",
    ""
) -join "`r`n"

$bodyBytes = $encoding.GetBytes($bodyLines)

try {
    $response = Invoke-WebRequest -Uri $UploadUrl -Method Post `
        -ContentType "multipart/form-data; boundary=$boundary" `
        -Body $bodyBytes
    Write-Host "Status: $($response.StatusCode)"
    Write-Host "Response: $($response.Content)"
}
catch {
    if ($_.Exception.Response) {
        Write-Host "Status: $([int]$_.Exception.Response.StatusCode)"
        $stream = $_.Exception.Response.GetResponseStream()
        if ($stream.CanSeek) { $stream.Position = 0 }
        $reader = New-Object System.IO.StreamReader($stream)
        $body = $reader.ReadToEnd()
        if ($body) {
            Write-Host "Response: $body"
        }
        else {
            Write-Host "Response: <empty body - check the server's own console/log output for the traceback>"
        }
    }
    else {
        Write-Error $_.Exception.Message
    }
    exit 1
}
