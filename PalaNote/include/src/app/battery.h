#pragma once

void  batteryInit();
float readBatteryVoltage();
int   batteryPercentFromVoltage(float v);
int   readBatteryPercent();
void  drawThickArcDot(int cx, int cy, int r, int deg, int thickness, uint8_t color);
void  drawArcRing(int cx, int cy, int r, int thickness, int percent);
void  drawBatteryRing(int percent);
void  drawStorageRing(int percent);
