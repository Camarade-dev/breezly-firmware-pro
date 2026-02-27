#pragma once

#define CURRENT_FIRMWARE_VERSION "1.0.25"
#define BUILD_ID __DATE__ " " __TIME__

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

// ---------- Backoff exponentiel (Wi-Fi / MQTT) ----------
// Wi-Fi: min 1s, max 5 min, facteur 2, jitter ±10%. Auth fail: min 30s pour éviter marteler la box.
#define BACKOFF_WIFI_MIN_MS           (1000UL)
#define BACKOFF_WIFI_MAX_MS           (5UL * 60UL * 1000UL)
#define BACKOFF_WIFI_FACTOR           2.0f
#define BACKOFF_WIFI_JITTER_PERCENT   10
#define BACKOFF_WIFI_AUTH_FAIL_MIN_MS (30UL * 1000UL)

// MQTT: min 2s, max 5 min, facteur 2, jitter ±10%.
#define BACKOFF_MQTT_MIN_MS           (2000UL)
#define BACKOFF_MQTT_MAX_MS           (5UL * 60UL * 1000UL)
#define BACKOFF_MQTT_FACTOR           2.0f
#define BACKOFF_MQTT_JITTER_PERCENT   10
