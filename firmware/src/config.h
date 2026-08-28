#pragma once
// User-editable settings.

// ---- WiFi -------------------------------------------------------------
#define WIFI_SSID      "your-wifi-name"
#define WIFI_PASSWORD  "your-wifi-password"
#define WIFI_CONNECT_TIMEOUT_MS  15000

// ---- Companion receiver server ----------------------------------------
// Where the device uploads finished recordings. This points at the Docker
// deployment on savage.local (see server/deploy.sh). arduino-esp32 generally
// resolves "*.local" mDNS hostnames out of the box, but mDNS reliability
// varies by router/network - if the device can't resolve it, replace this
// with savage.local's plain IP address instead (find it with `ping
// savage.local` from another machine on the same network, or check your
// router's client list).
#define UPLOAD_SERVER_URL  "http://savage.local:8787/upload"

// ---- Audio --------------------------------------------------------------
#define SAMPLE_RATE_HZ   16000   // 16kHz mono is plenty for speech and keeps files small
#define SAMPLE_BITS      16
#define AUDIO_CHANNELS   1

// ---- Behavior -----------------------------------------------------------
#define AUTO_UPLOAD_WHEN_IDLE  true   // if true, device tries to flush pending recordings to WiFi whenever it's idle and not recording
#define UPLOAD_RETRY_INTERVAL_MS  (5UL * 60UL * 1000UL)  // don't hammer WiFi if the server is unreachable

// ---- Menu ---------------------------------------------------------------
#define MENU_IDLE_TIMEOUT_MS  (8UL * 1000UL)   // auto-return to idle if the menu sits untouched this long
#define DEVICE_VERSION  "0.2"
