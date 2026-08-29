#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// The LCD and SD card are wired to the same SPI bus pins (see pins.h). A
// single loop() task never needed to arbitrate between them - one task
// can't race itself. Now that upload_worker.h runs a second FreeRTOS task
// doing SD reads in the background, any SPI traffic - an LCD draw from
// loop(), or an SD read/write from either task - has to hold this mutex for
// as long as the transaction takes, or the two drivers (Arduino_ESP32SPI
// for the LCD, SD.h for the card) can corrupt each other's bytes on the
// shared MOSI/SCLK lines. Never hold it across a network wait or anything
// else that blocks for a while - only around the actual bus access.
extern SemaphoreHandle_t g_spiBusMutex;

// Must be called once from setup(), before the upload worker task starts -
// every other use of the mutex assumes it already exists.
void spiBusMutexBegin();

class SpiBusGuard {
 public:
  SpiBusGuard() { xSemaphoreTake(g_spiBusMutex, portMAX_DELAY); }
  ~SpiBusGuard() { xSemaphoreGive(g_spiBusMutex); }
  SpiBusGuard(const SpiBusGuard &) = delete;
  SpiBusGuard &operator=(const SpiBusGuard &) = delete;
};
