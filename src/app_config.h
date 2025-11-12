#pragma once

#define CURRENT_FIRMWARE_VERSION "1.0.11"
static const char* FW_MANIFEST_URL =
   "https://breezly-backend.onrender.com/api/ota/manifest?model=wroom32e&channel=prod";
#define LED_PIN   13
#define LED_COUNT 1
#define PMS_ALWAYS_ON   false
// 6h => 6UL*60UL*60UL*1000UL; ici 5 min pour tester
#define OTA_CHECK_INTERVAL_MS (12UL*60UL*60UL*1000UL)
