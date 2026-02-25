#pragma once

#define CURRENT_FIRMWARE_VERSION "1.0.24"

#ifdef APP_ENV_DEV
// Canal DEV → manifest dev
static const char* FW_MANIFEST_URL =
  "https://Camarade-dev.github.io/breezly-firmware-dist/firmware/esp32/wroom32e/dev/latest.json";
#else
// Canal PROD → manifest prod
static const char* FW_MANIFEST_URL =
  "https://Camarade-dev.github.io/breezly-firmware-dist/firmware/esp32/wroom32e/prod/latest.json";
#endif

#define LED_PIN   13
#define LED_COUNT 1
#define PMS_ALWAYS_ON   false
#define OTA_CHECK_INTERVAL_MS (12UL*60UL*60UL*1000UL)
