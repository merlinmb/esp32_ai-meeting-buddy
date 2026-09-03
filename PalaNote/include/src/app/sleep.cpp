#include "Arduino.h"
#include "../../config.h"
#include "../../globals.h"
#include "../../types.h"
#include "sleep.h"
#include "ui.h"
#include "network.h"
#include "../../sounds.h"
#include "WiFi.h"
#include "driver/gpio.h"

extern "C" {
#include "../../src/audio/audio_bsp.h"
}

void resetActivity() {
  lastActivityMs = millis();
}

void enterUltraSleep() {
  showUltraSleepScreen();
  delay(120);

  stopTransferMode();

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  audio_playback_set_vol(0);
  palaSoundSetEnabled(false);

  board.VBAT_POWER_ON();

  // Deep sleep resets GPIO output state by default, which would let the
  // VBAT latch (GPIO17/BAT_Control) glitch open mid-transition and brown
  // out the 3V3 rail when running on battery alone (USB masks this by
  // feeding VSYS independently). Holding the pin keeps the latch driven
  // HIGH through sleep and the wake reset, until setup() re-asserts it
  // and releases the hold.
  gpio_hold_en((gpio_num_t)VBAT_PWR_PIN);
  gpio_deep_sleep_hold_en();

  uint64_t wakeMask = (1ULL << BTN_REC) | (1ULL << BTN_PWR);
  esp_sleep_enable_ext1_wakeup(wakeMask, ESP_EXT1_WAKEUP_ANY_LOW);

  delay(50);
  esp_deep_sleep_start();
}
