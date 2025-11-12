#include "wifi_enterprise.h"
#include "../core/globals.h"
#include "../net/sntp_utils.h"
#include "../ble/provisioning.h"
#include <WiFi.h>
#include "esp_task_wdt.h"
#include "wifi_status_helpers.h"
extern "C" {
  #include "esp_wifi.h"
  #include "esp_event.h"
  #include "esp_eap_client.h"   // API Enterprise moderne (ESP-IDF 5.x)
}
extern bool bleInited;
extern void restartBLEAdvertising();

static volatile int s_lastDiscReasonEap = -1;
static esp_event_handler_instance_t s_discInstEap = nullptr;

static void onStaDiscEap(void*, esp_event_base_t, int32_t, void* data) {
  auto* ev = (wifi_event_sta_disconnected_t*)data;
  s_lastDiscReasonEap = ev ? ev->reason : -1;
  Serial.printf("[EAP] STA_DISCONNECTED reason=%d\n", s_lastDiscReasonEap);
}

// ─────────────────────────── Hooks no-op (si tu veux pauser d’autres stacks)
static inline void pauseOtherNetWork() {}
static inline void resumeOtherNetWork() {}

// ─────────────────────────── Log de déconnexion (diag)
static void onStaDisc(void*, esp_event_base_t, int32_t, void* data) {
  auto* ev = (wifi_event_sta_disconnected_t*)data;
  Serial.printf("[EAP] STA_DISCONNECTED reason=%d\n", ev->reason);
}
static esp_event_handler_instance_t s_discInst = nullptr;

// ─────────────────────────── Certificat CA embarqué
//   Fichier PEM à placer dans: src/certs/ca_rezoleo.pem
//   Et ajouter à platformio.ini:
//     board_build.embed_txtfiles = src/certs/ca_rezoleo.pem
extern const uint8_t _binary_src_certs_ca_rezoleo_pem_start[];
extern const uint8_t _binary_src_certs_ca_rezoleo_pem_end[];

// ─────────────────────────── Connexion WPA2-Enterprise (PEAP/MSCHAPv2)
bool connectToWiFiEnterprise() {
  if (wifiSSID.isEmpty() || eapUsername.isEmpty() || eapPassword.isEmpty()) {
    Serial.println("[EAP] champs manquants (ssid/user/pass)");
    provSet("status", "missing_fields");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, true);
  esp_wifi_set_ps(WIFI_PS_NONE);

  // handler (1 fois)
  if (!s_discInstEap) {
    ESP_ERROR_CHECK( esp_event_handler_instance_register(
      WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &onStaDiscEap, nullptr, &s_discInstEap) );
  }

  // Nettoyage EAP
  esp_wifi_sta_enterprise_disable();
  #ifdef esp_eap_client_clear_identity
    esp_eap_client_clear_identity();
  #endif
  #ifdef esp_eap_client_clear_username
    esp_eap_client_clear_username();
  #endif
  #ifdef esp_eap_client_clear_password
    esp_eap_client_clear_password();
  #endif
  #ifdef esp_eap_client_clear_new_password
    esp_eap_client_clear_new_password();
  #endif
  #ifdef esp_eap_client_clear_ca_cert
    esp_eap_client_clear_ca_cert();
  #endif
  #ifdef esp_eap_client_clear_cert_key
    esp_eap_client_clear_cert_key();
  #endif

  // Identités + MDP + CA (comme avant)
  const String outer = eapAnon.length() ? eapAnon : "ano@rezoleo.fr";
  ESP_ERROR_CHECK( esp_eap_client_set_identity((const uint8_t*)outer.c_str(), outer.length()) );
  ESP_ERROR_CHECK( esp_eap_client_set_username((const uint8_t*)eapUsername.c_str(), eapUsername.length()) );
  ESP_ERROR_CHECK( esp_eap_client_set_password((const uint8_t*)eapPassword.c_str(), eapPassword.length()) );
  esp_eap_client_set_disable_time_check(true);

  extern const uint8_t _binary_src_certs_ca_rezoleo_pem_start[];
  extern const uint8_t _binary_src_certs_ca_rezoleo_pem_end[];
  ESP_ERROR_CHECK( esp_eap_client_set_ca_cert(
      (const unsigned char*)_binary_src_certs_ca_rezoleo_pem_start,
      (int)(_binary_src_certs_ca_rezoleo_pem_end - _binary_src_certs_ca_rezoleo_pem_start)
  ) );

  esp_err_t er = esp_wifi_sta_enterprise_enable();
  if (er == ESP_ERR_WIFI_NOT_INIT) {
    WiFi.mode(WIFI_STA);
    er = esp_wifi_sta_enterprise_enable();
  }
  ESP_ERROR_CHECK(er);

  Serial.printf("[EAP] Connexion à '%s'…\n", wifiSSID.c_str());
  provSet("status", "connecting");

  WiFi.begin(wifiSSID.c_str());

  // Attente ~20 s
  bool ok = false;
  for (int i = 0; i < 80; ++i) {
    if (WiFi.status() == WL_CONNECTED) { ok = true; break; }
    esp_task_wdt_reset();
    vTaskDelay(250 / portTICK_PERIOD_MS);
    if ((i % 2) == 0) Serial.print(".");
  }
  Serial.println();

  if (ok) {
    wifiConnected = true;
    breezly_on_wifi_ok();
    Serial.printf("[EAP] OK IP=%s  RSSI=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());

    // 1) Wi-Fi OK
    provSet("status", "wifi_ok");

    // 2) Internet ?
    bool inet = checkInternetReachable();
    if (!inet) {
      Serial.println("[EAP] Internet unreachable");
      provSet("status", "inet_unreachable");
      return false;
    }
    breezly_on_inet_ok();
    provSet("status", "inet_ok");
    // 3) Final
    provisioningNotifyConnected();
    startSNTPAfterConnected();
    return true;
  }

  // Échec : préciser la cause
  wifiConnected = false;
  const char* st = mapDiscReasonToStatus(s_lastDiscReasonEap);
  provSet("status", st);
  if (s_lastDiscReasonEap == WIFI_REASON_802_1X_AUTH_FAILED) {
    breezly_on_wifi_auth_failed();
  } else if (s_lastDiscReasonEap == WIFI_REASON_NO_AP_FOUND ||
            s_lastDiscReasonEap == WIFI_REASON_ASSOC_EXPIRE ||
            s_lastDiscReasonEap == WIFI_REASON_HANDSHAKE_TIMEOUT) {
    breezly_on_wifi_assoc_timeout();
  }
  if (bleInited) restartBLEAdvertising();
  return false;
}
