#pragma once
#include "Arduino.h"

// Wi-Fi profiles and playback volume kept in NVS (Preferences), configured
// over USB serial. With no stored profiles every call behaves exactly as the
// original firmware did: connect to WIFI_SSID / WIFI_PASS from secrets.h.

#define WIFI_MAX_PROFILES 5

// Starts a connection attempt (call instead of WiFi.begin(WIFI_SSID, WIFI_PASS)).
void settingsWifiBegin();

// Playback volume, 10..100. Defaults to the previous hard-coded 85.
uint8_t settingsVolume();

// Handles pending "WIFI+ ssid|pass", "WIFI-ssid", "WIFI?", "VOL n", "STATUS"
// lines from the USB serial port. Cheap when nothing is waiting.
void settingsPollSerial();
