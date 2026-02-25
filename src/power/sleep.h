#pragma once
#include <Arduino.h>
#include <WiFi.h>
extern bool bleInited;
bool otaIsInProgress(void);  // ota/ota.h
extern "C" {
  #include "esp_wifi.h"   // <-- déclare esp_wifi_set_ps / wifi_ps_type_t
}
static inline void enterModemSleep(bool enable){
  // Pendant OTA / provisioning, on évite d’endormir le modem
  if (otaIsInProgress() || bleInited) return;
  esp_wifi_set_ps(enable ? WIFI_PS_MAX_MODEM : WIFI_PS_NONE);
}

// Si tu veux des micro-siestes non bloquantes
static inline void lightNapMs(uint32_t ms){
  // Ici simple vTaskDelay pour ne pas perturber les tâches MQTT/LED
  // (Tu peux remplacer par esp_light_sleep_start si tu veux aller plus loin)
  uint32_t end = millis() + ms;
  while ((int32_t)(end - millis()) > 0){
    vTaskDelay(50/portTICK_PERIOD_MS);
  }
}

static inline void deepSleepForMs(uint64_t ms){
#if USE_DEEP_SLEEP
  // Coupe proprement avant
  WiFi.disconnect(true, true);
  // LEDs off
  // pms -> sleep déjà fait
  esp_sleep_enable_timer_wakeup(ms * 1000ULL);
  esp_deep_sleep_start();  // ne revient pas
#else
  (void)ms;
#endif
}
