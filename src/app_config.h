#pragma once

#define CURRENT_FIRMWARE_VERSION "1.0.1"
static const char* FW_MANIFEST_URL =
  "https://breezly-backend.onrender.com/firmware/esp32/wroom32e/prod/manifest.json";

#define LED_PIN   13
#define LED_COUNT 1

// 6h => 6UL*60UL*60UL*1000UL; ici 5 min pour tester
#define OTA_CHECK_INTERVAL_MS (1UL*5UL*60UL*1000UL)
