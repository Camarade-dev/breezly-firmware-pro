#include <Arduino.h>
#include <time.h>
extern "C" {
  #include "esp_sntp.h"
}

static bool sntpStarted = false;

void startSNTPAfterConnected() {
  if (sntpStarted) return;
  sntpStarted = true;

  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();

  for (int i = 0; i < 40; ++i) {
    time_t now = time(nullptr);
    if (now > 1700000000) break;
    delay(250);
  }
}
