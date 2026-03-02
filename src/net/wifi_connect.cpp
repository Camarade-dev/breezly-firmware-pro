#include "wifi_connect.h"
#include "../core/log.h"
#include <WiFi.h>
#include "esp_task_wdt.h"
extern "C" {
  #include "esp_wifi.h"
}
#include "../power/wifi_txpower.h"
#include "../core/globals.h"
#include "../core/backoff.h"
#include "../app_config.h"
#include "../net/sntp_utils.h"
#include "../net/mqtt_bus.h"
#include "../ble/provisioning.h"
#include "../led/led_status.h"
#include "wifi_enterprise.h"   // connectToWiFiEnterprise()
#include "wifi_status_helpers.h"

static const BackoffConfig s_wifiBackoffConfig = {
  BACKOFF_WIFI_MIN_MS,
  BACKOFF_WIFI_MAX_MS,
  BACKOFF_WIFI_FACTOR,
  BACKOFF_WIFI_JITTER_PERCENT,
  BACKOFF_WIFI_INTERMEDIATE_MAX_MS
};
static Backoff s_wifiBackoff(s_wifiBackoffConfig);

static wifi_backoff::Reason mapDiscReasonToBackoffReason(int reason) {
  if (reason == WIFI_REASON_AUTH_EXPIRE || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
      reason == WIFI_REASON_802_1X_AUTH_FAILED || reason == WIFI_REASON_MIC_FAILURE ||
      reason == WIFI_REASON_AUTH_FAIL)
    return wifi_backoff::AuthFail;
  if (reason == WIFI_REASON_NO_AP_FOUND || reason == WIFI_REASON_ASSOC_LEAVE ||
      reason == WIFI_REASON_ASSOC_EXPIRE || reason == WIFI_REASON_ASSOC_TOOMANY)
    return wifi_backoff::NoSsid;
  if (reason == WIFI_REASON_HANDSHAKE_TIMEOUT)
    return wifi_backoff::Timeout;
  return wifi_backoff::Other;
}

void wifiBackoffReset() { s_wifiBackoff.reset(); }
bool wifiBackoffShouldAttempt() { return s_wifiBackoff.shouldAttempt(millis()); }

// === Version PSK (copiée de ton ancien code) ===
static volatile int s_lastDiscReasonPsk = -1;
static esp_event_handler_instance_t s_discInstPsk = nullptr;

static void onStaDiscPsk(void*, esp_event_base_t, int32_t, void* data) {
  auto* ev = (wifi_event_sta_disconnected_t*)data;
  s_lastDiscReasonPsk = ev ? ev->reason : -1;
  LOGD("WiFi", "STA_DISCONNECTED reason=%d", s_lastDiscReasonPsk);
  wifiConnected = false;  // lien perdu (ex. partage de connexion coupé)
  mqtt_bus_reset_backoff_on_wifi_lost();  // reconnexion rapide au retour du Wi‑Fi
  updateLedState(LED_BAD);  // rouge clignotant tout de suite, sans attendre la loop
}
static void ensureDefaultEventLoop() {
  static bool ready = false;
  if (ready) return;
  esp_err_t e = esp_event_loop_create_default();
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
    LOGE("EVT", "create_default failed: %d", e);
  }
  ready = true;
}
static bool connectToWiFiPSK() {
  if (wifiSSID.isEmpty() || wifiPassword.isEmpty()){
    LOGW("WiFi", "SSID/PWD manquants");
    provSet("status", "missing_fields");
    return false;
  }

  ensureDefaultEventLoop();
  if (!s_discInstPsk) {
    esp_err_t e = esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &onStaDiscPsk, nullptr, &s_discInstPsk);
    if (e != ESP_OK) {
      LOGE("EVT", "register STA_DISCONNECTED failed: %d", e);
      // ne surtout pas ESP_ERROR_CHECK ici → pas de reboot
    }
  }
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  provSet("status", "connecting");
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  LOGD("WiFi", "Connexion à '%s'...", wifiSSID.c_str());
  LOGI("WiFi", "connecting...");

  const uint32_t timeoutMs = 15000;  // timeout d'une seule tentative (pas le backoff)
  const uint32_t deadline  = millis() + timeoutMs;
  while (WiFi.status() != WL_CONNECTED && (int32_t)(deadline - millis()) > 0) {
    esp_task_wdt_reset();
    vTaskDelay(120 / portTICK_PERIOD_MS);
#if BREEZLY_LOG_LEVEL >= BREEZLY_LOG_LEVEL_DEBUG
    if (((millis()/600) % 2) == 0) Serial.print(".");
#endif
  }
#if BREEZLY_LOG_LEVEL >= BREEZLY_LOG_LEVEL_DEBUG
  Serial.println();
#endif

  if (WiFi.status()==WL_CONNECTED){
  s_wifiBackoff.reset();
  wifiFailCount = 0;
  LOGI("WiFi", "OK IP=%s RSSI=%d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
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
    LOGW("WiFi", "Internet unreachable");
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
  wifiFailCount++;
  wifi_backoff::Reason reason = mapDiscReasonToBackoffReason(s_lastDiscReasonPsk);
  uint32_t effectiveMin = wifi_backoff::effectiveMinForReason(reason, BACKOFF_WIFI_AUTH_FAIL_MIN_MS);
  s_wifiBackoff.onFailure(millis(), effectiveMin);
  LOGW("WiFi", "fail reason=%d backoff next in %lu ms", s_lastDiscReasonPsk, (unsigned long)s_wifiBackoff.lastDelayMs());
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
    if (ok) {
      s_wifiBackoff.reset();
      wifiFailCount = 0;
    } else {
      wifiFailCount++;
      s_wifiBackoff.onFailure(millis(), 0);  // EAP: pas de policy auth spécifique ici
      ledOnProvisioningError();
      if (bleInited) restartBLEAdvertising();
    }
    return ok;
  }
  bool ok = connectToWiFiPSK();
  if (!ok && bleInited) restartBLEAdvertising();
  return ok;
}
