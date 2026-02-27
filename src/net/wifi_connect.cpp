#include "wifi_connect.h"
#include <WiFi.h>
#include "esp_task_wdt.h"
extern "C" {
  #include "esp_wifi.h"
}
#include "../power/wifi_txpower.h"
#include "../core/globals.h"
#include "../net/sntp_utils.h"
#include "../net/mqtt_bus.h"
#include "../ble/provisioning.h"
#include "../led/led_status.h"
#include "wifi_enterprise.h"   // connectToWiFiEnterprise()
#include "wifi_status_helpers.h"

// === Version PSK (copiée de ton ancien code) ===
static volatile int s_lastDiscReasonPsk = -1;
static esp_event_handler_instance_t s_discInstPsk = nullptr;

static void onStaDiscPsk(void*, esp_event_base_t, int32_t, void* data) {
  auto* ev = (wifi_event_sta_disconnected_t*)data;
  s_lastDiscReasonPsk = ev ? ev->reason : -1;
  Serial.printf("[WiFi][PSK] STA_DISCONNECTED reason=%d\n", s_lastDiscReasonPsk);
}
static void ensureDefaultEventLoop() {
  static bool ready = false;
  if (ready) return;
  esp_err_t e = esp_event_loop_create_default();
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
    Serial.printf("[EVT] create_default failed: %d\n", e);
  }
  ready = true;
}
static bool connectToWiFiPSK() {
  if (wifiSSID.isEmpty() || wifiPassword.isEmpty()){
    Serial.println("[WiFi] SSID/PWD manquants");
    provSet("status", "missing_fields");
    return false;
  }

  ensureDefaultEventLoop();
  if (!s_discInstPsk) {
    esp_err_t e = esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &onStaDiscPsk, nullptr, &s_discInstPsk);
    if (e != ESP_OK) {
      Serial.printf("[EVT] register STA_DISCONNECTED failed: %d\n", e);
      // ne surtout pas ESP_ERROR_CHECK ici → pas de reboot
    }
  }
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  provSet("status", "connecting");
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  Serial.printf("[WiFi] Connexion à '%s'...\n", wifiSSID.c_str());

  const uint32_t timeoutMs = 15000;
  const uint32_t deadline  = millis() + timeoutMs;
  while (WiFi.status() != WL_CONNECTED && (int32_t)(deadline - millis()) > 0) {
    esp_task_wdt_reset();
    vTaskDelay(120 / portTICK_PERIOD_MS);
    if (((millis()/600) % 2) == 0) Serial.print(".");
  }
  Serial.println();

  if (WiFi.status()==WL_CONNECTED){
  Serial.printf("[WiFi] OK IP=%s RSSI=%d\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  wifiAutoTxPower();
  wifiConnected = true;
  {
    char ctx[120];
    snprintf(ctx, sizeof(ctx), "{\"ip\":\"%s\",\"rssi\":%d}",
             WiFi.localIP().toString().c_str(), WiFi.RSSI());
    mqtt_telemetry_emit("WIFI_CONNECT_OK", ctx);
  }
  breezly_on_wifi_ok();
  // Étape 1 : Wi-Fi OK 
  provSet("status", "wifi_ok");

  // 🔑 Lance la sync horloge tout de suite (évite le poulet–œuf)
  startSNTPAfterConnected();

  // Étape 2 : Internet ? (time-agnostic)
  bool inet = checkInternetReachable();
  if (!inet) {
    Serial.println("[WiFi] Internet unreachable");
    provisioningSetStatus("{\"status\":\"inet_unreachable\"}");
    // ne PAS appeler breezly_on_inet_ok() ici
  } else {
    breezly_on_inet_ok();
    provisioningSetStatus("{\"status\":\"inet_ok\"}");
  }

  // Étape 3 : final
  provisioningNotifyConnected();
  return true;
}



  // ÉCHEC: on tente d’éclairer la cause
  wifiConnected = false;
  char ctx[80];
  snprintf(ctx, sizeof(ctx), "{\"reason\":%d,\"retryCount\":%u}", s_lastDiscReasonPsk, (unsigned)wifiFailCount);
  mqtt_telemetry_emit("WIFI_CONNECT_FAIL", ctx);
  const char* st = mapDiscReasonToStatus(s_lastDiscReasonPsk);
  provSet("status", st);
  // Traduit quelques causes communes en hooks dédiés
  if (s_lastDiscReasonPsk == WIFI_REASON_AUTH_EXPIRE ||
      s_lastDiscReasonPsk == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
      s_lastDiscReasonPsk == WIFI_REASON_802_1X_AUTH_FAILED ||
      s_lastDiscReasonPsk == WIFI_REASON_MIC_FAILURE ||
      s_lastDiscReasonPsk == WIFI_REASON_AUTH_FAIL) {
    breezly_on_wifi_auth_failed();
  } else if (s_lastDiscReasonPsk == WIFI_REASON_NO_AP_FOUND ||
            s_lastDiscReasonPsk == WIFI_REASON_ASSOC_LEAVE ||
            s_lastDiscReasonPsk == WIFI_REASON_ASSOC_EXPIRE ||
            s_lastDiscReasonPsk == WIFI_REASON_ASSOC_TOOMANY ||
            s_lastDiscReasonPsk == WIFI_REASON_HANDSHAKE_TIMEOUT) {
    breezly_on_wifi_assoc_timeout();
  }

  ledOnProvisioningError();
  if (bleInited) restartBLEAdvertising();
  return false;
}

bool connectToWiFi(){
  if (wifiAuthType == WIFI_CONN_EAP_PEAP_MSCHAPV2) {
    bool ok = connectToWiFiEnterprise();
    if (!ok) {
      ledOnProvisioningError();
      if (bleInited) restartBLEAdvertising();
    }
    return ok;
  }
  bool ok = connectToWiFiPSK();
  if (!ok && bleInited) restartBLEAdvertising();
  return ok;
}
