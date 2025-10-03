#include <Arduino.h>
#include <time.h>
extern "C" {
  #include "esp_sntp.h"
}

static bool sntpStarted = false;

bool timeIsSane() {
  // > 2023-11-14 environ
  return time(nullptr) > 1700000000;
}
void stopSNTP(){ if (sntpStarted) { esp_sntp_stop(); sntpStarted = false; } }
void startSNTPAfterConnected() {
  if (sntpStarted) return;
  sntpStarted = true;

  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  // Optionnel: des serveurs de secours
  // esp_sntp_setservername(1, "time.google.com");
  // esp_sntp_setservername(2, "fr.pool.ntp.org");
  esp_sntp_init();

  // Attente active (max ~10 s). Augmente si besoin.
  for (int i = 0; i < 80; ++i) {  // 80*250ms ≈ 20 s
    if (timeIsSane())
    { 
      esp_sntp_stop(); sntpStarted=false;
      break;
    }
    delay(250);
  }
}
