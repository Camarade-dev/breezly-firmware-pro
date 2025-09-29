#include "wifi_connect.h"
#include <WiFi.h>
#include "esp_task_wdt.h"
extern "C" {
  #include "esp_wifi.h"
}
#include "../core/globals.h"
#include "../net/sntp_utils.h"
#include "../ble/provisioning.h"
#include "../led/led_status.h"
#include "wifi_enterprise.h"   // connectToWiFiEnterprise()
// === Version PSK (copiée de ton ancien code) ===
static bool connectToWiFiPSK(){
  if (wifiSSID.isEmpty() || wifiPassword.isEmpty()){
    Serial.println("[WiFi] SSID/PWD manquants");
    provisioningSetStatus("{\"status\":\"missing_fields\"}");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);                 // reset complet
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  Serial.printf("[WiFi] Connexion à '%s'...\n", wifiSSID.c_str());

  const uint32_t timeoutMs = 15000;           // 15 s
  const uint32_t deadline  = millis() + timeoutMs;
  while (WiFi.status() != WL_CONNECTED && (int32_t)(deadline - millis()) > 0) {
    esp_task_wdt_reset();
    vTaskDelay(100 / portTICK_PERIOD_MS);
    if (((millis()/500) % 2) == 0) Serial.print(".");
  }
  Serial.println();

  if (WiFi.status()==WL_CONNECTED){
    Serial.printf("[WiFi] OK IP=%s RSSI=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    wifiConnected = true;
    provisioningNotifyConnected();
    startSNTPAfterConnected();
    return true;
  } else {
    Serial.println("[WiFi] ÉCHEC (timeout)");
    wifiConnected = false;
    provisioningSetStatus("{\"status\":\"error\"}");
    if (bleInited) restartBLEAdvertising();
    return false;
  }
}

bool connectToWiFi(){
  if (wifiAuthType == WIFI_CONN_EAP_PEAP_MSCHAPV2) {
    bool ok = connectToWiFiEnterprise();
    if (ok) provisioningNotifyConnected();
    else {
      provisioningSetStatus("{\"status\":\"error\"}");
      if (bleInited) restartBLEAdvertising();
    }
    return ok;
  }
  return connectToWiFiPSK();
}
