// power/wifi_txpower.h
#pragma once
#include <WiFi.h>

static inline void wifiAutoTxPower(){
  int r = WiFi.RSSI(); // dBm
  // seuils simples (à ajuster au besoin)
  wifi_power_t pwr =
      (r > -55) ? WIFI_POWER_8_5dBm  :   // près de l’AP
      (r > -65) ? WIFI_POWER_15dBm   :   // moyen
                  WIFI_POWER_19_5dBm;    // loin
  WiFi.setTxPower(pwr);
}
