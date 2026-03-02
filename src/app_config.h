#pragma once

#define CURRENT_FIRMWARE_VERSION "1.0.26"
#define BUILD_ID __DATE__ " " __TIME__

// Canal manifest OTA selon build : backend Render uniquement (BREEZLY_DEV → dev, sinon prod).
#if defined(BREEZLY_DEV)
static const char* FW_MANIFEST_URL =
  "https://breezly-backendweb.onrender.com/firmware/esp32/wroom32e/dev/latest.json";
#else
static const char* FW_MANIFEST_URL =
  "https://breezly-backend.onrender.com/firmware/esp32/wroom32e/prod/latest.json";
#endif

#define LED_PIN   13
#define LED_COUNT 1
#define PMS_ALWAYS_ON   false

// ---------- I2C bus (capteurs AHT21 / ENS160) — robustesse ----------
#ifndef I2C_BUS_TIMEOUT_MS
#define I2C_BUS_TIMEOUT_MS         (500UL)   // Timeout par transaction Wire (évite blocage)
#endif
#ifndef I2C_BUS_RESET_AFTER_FAILURES
#define I2C_BUS_RESET_AFTER_FAILURES  (3)    // Après N échecs consécutifs de lecture → Wire.end/begin + re-init capteurs
#endif

// ---------- Sanity checks AQI/TVOC/eCO2 (payload toujours envoyé ; flag pour le backend) ----------
#ifndef SANITY_AQI_MIN
#define SANITY_AQI_MIN   (1)       // ENS160 AQI index 1–5
#endif
#ifndef SANITY_AQI_MAX
#define SANITY_AQI_MAX   (5)
#endif
#ifndef SANITY_TVOC_MAX_PPB
#define SANITY_TVOC_MAX_PPB  (20000UL)  // TVOC ppb : au-delà = possible erreur / environnement extrême
#endif
#ifndef SANITY_ECO2_MIN_PPM
#define SANITY_ECO2_MIN_PPM  (300)   // eCO2 ppm : en dessous = warmup ou erreur
#endif
#ifndef SANITY_ECO2_MAX_PPM
#define SANITY_ECO2_MAX_PPM  (10000) // eCO2 ppm : au-delà = possible erreur
#endif

#define OTA_CHECK_INTERVAL_MS (12UL*60UL*60UL*1000UL)

// ---------- Backoff exponentiel (Wi-Fi / MQTT) ----------
// Wi-Fi: min 1s, palier 1 min, max 5 min, facteur 2, jitter ±10%. Auth fail: min 30s pour éviter marteler la box.
#define BACKOFF_WIFI_MIN_MS                 (1000UL)
#define BACKOFF_WIFI_INTERMEDIATE_MAX_MS    (60UL * 1000UL)   // 1 min avant d'aller vers 5 min
#define BACKOFF_WIFI_MAX_MS                 (5UL * 60UL * 1000UL)
#define BACKOFF_WIFI_FACTOR                 2.0f
#define BACKOFF_WIFI_JITTER_PERCENT         10
#define BACKOFF_WIFI_AUTH_FAIL_MIN_MS      (30UL * 1000UL)

// MQTT: min 2s, palier 1 min, max 5 min, facteur 2, jitter ±10%.
#define BACKOFF_MQTT_MIN_MS                 (2000UL)
#define BACKOFF_MQTT_INTERMEDIATE_MAX_MS    (60UL * 1000UL)   // 1 min
#define BACKOFF_MQTT_MAX_MS                 (5UL * 60UL * 1000UL)
#define BACKOFF_MQTT_FACTOR                 2.0f
#define BACKOFF_MQTT_JITTER_PERCENT         10

// ---------- Control payload v1 (anti-replay + HMAC) ----------
#if defined(BREEZLY_PROD)
#define CTRL_REQUIRE_SIG              1
#define CTRL_ALLOW_UNSIGNED           0
#define CTRL_FACTORY_RESET_ENABLED    0
#else
#define CTRL_REQUIRE_SIG              0
#define CTRL_ALLOW_UNSIGNED          1
#define CTRL_FACTORY_RESET_ENABLED    1
#endif
#define CTRL_MAX_SKEW_SEC             (300)
#define CTRL_CMDID_RING_SIZE          (16)
#define CTRL_RATE_LIMIT_MIN_MS        (2000UL)   // 1 cmd / 2s
#define CTRL_RATE_LIMIT_BURST         (3)
#define CTRL_FACTORY_RESET_REQUIRE_HOLD_MS  (5000)
#define CTRL_PAYLOAD_MAX_BYTES        (1024)
#define CTRL_SET_WIFI_SSID_MAX        (64)
#define CTRL_SET_WIFI_PASSWORD_MAX   (128)
