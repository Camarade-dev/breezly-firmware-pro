#include "wifi_enterprise.h"
#include "../core/log.h"
#include "../core/globals.h"
#include "../net/sntp_utils.h"
#include "mqtt_bus.h"
#include "../led/led_status.h"
#include "../ble/provisioning.h"
#include <WiFi.h>
#include "esp_task_wdt.h"
#include "wifi_status_helpers.h"
extern "C" {
  #include "esp_wifi.h"
  #include "esp_event.h"
  #include "esp_eap_client.h"
}
extern bool bleInited;
extern void restartBLEAdvertising();

static volatile int  s_lastDiscReasonEap = -1;
static volatile bool s_connectingEap     = false;   // vrai pendant la fenêtre de connexion
static esp_event_handler_instance_t s_discInstEap = nullptr;

// ─────────────────────────── Handler STA_DISCONNECTED
// N'agit QUE si on est en train de connecter ou déjà connecté.
// Évite le spam de logs/actions lors des retries automatiques au boot.
static void onStaDiscEap(void*, esp_event_base_t, int32_t, void* data) {
  auto* ev = (wifi_event_sta_disconnected_t*)data;
  int reason = ev ? (int)ev->reason : -1;

  if (!s_connectingEap && !wifiConnected) {
    // Retry automatique du stack Arduino en dehors d'une tentative explicite → ignorer
    LOGD("EAP", "STA_DISCONNECTED reason=%d (ignoré, pas de connexion active)", reason);
    return;
  }

  s_lastDiscReasonEap = reason;
  LOGD("EAP", "STA_DISCONNECTED reason=%d", reason);

  if (wifiConnected) {
    // Perte de connexion en cours d'utilisation
    wifiConnected = false;
    mqtt_bus_reset_backoff_on_wifi_lost();
    updateLedState(LED_BAD);
  }
}

// ─────────────────────────── Détachement du handler (mode BLE pur)
void wifi_enterprise_detach_disc_handler() {
  if (s_discInstEap) {
    esp_event_handler_instance_unregister(
      WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, s_discInstEap);
    s_discInstEap = nullptr;
  }
}

// ─────────────────────────── Certificat CA embarqué
extern const uint8_t _binary_src_certs_ca_rezoleo_pem_start[];
extern const uint8_t _binary_src_certs_ca_rezoleo_pem_end[];

// ─────────────────────────── Helper : "anonymous@domaine" depuis username
static String outerFromUsername(const String& username) {
  int at = username.indexOf('@');
  if (at >= 0) return "anonymous" + username.substring(at);
  return "anonymous";
}

// ─────────────────────────── Reset propre du stack WiFi
static void wifiHardReset() {
  WiFi.setAutoReconnect(false);   // ← stoppe les retries automatiques
  WiFi.disconnect(true, true);    // true,true = déconnecte + efface creds NVS
  WiFi.mode(WIFI_OFF);
  vTaskDelay(300 / portTICK_PERIOD_MS);
  WiFi.mode(WIFI_STA);
  vTaskDelay(100 / portTICK_PERIOD_MS);
  esp_wifi_set_ps(WIFI_PS_NONE);
}

// ─────────────────────────── Connexion WPA2-Enterprise (PEAP/MSCHAPv2)
bool connectToWiFiEnterprise() {
  if (wifiSSID.isEmpty() || eapUsername.isEmpty() || eapPassword.isEmpty()) {
    LOGW("EAP", "champs manquants (ssid/user/pass)");
    provSet("status", "missing_fields");
    return false;
  }

  // ── 1. Reset propre du stack ────────────────────────────────────────────
  s_connectingEap     = false;
  s_lastDiscReasonEap = -1;
  wifiHardReset();

  // ── 2. Handler STA_DISCONNECTED (enregistré une seule fois) ────────────
  if (!s_discInstEap) {
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
      &onStaDiscEap, nullptr, &s_discInstEap));
  }

  // ── 3. Nettoyage EAP ───────────────────────────────────────────────────
  esp_wifi_sta_enterprise_disable();
  esp_eap_client_clear_identity();
  esp_eap_client_clear_username();
  esp_eap_client_clear_password();
  esp_eap_client_clear_new_password();
  esp_eap_client_clear_ca_cert();

  // ── 4. Outer identity ──────────────────────────────────────────────────
  // Priorité : eapAnon explicite > dérivé du username
  // Mode insecure : outer = inner (skip la dérivation anonyme)
  String outer;
  if (eapInsecure) {
    outer = eapIdentity.length() ? eapIdentity : eapUsername;
  } else {
    outer = eapAnon.length() ? eapAnon : outerFromUsername(eapUsername);
  }

  LOGI("EAP", "outer='%s' inner='%s' insecure=%d",
       outer.c_str(), eapUsername.c_str(), (int)eapInsecure);

  ESP_ERROR_CHECK(esp_eap_client_set_identity(
      (const uint8_t*)outer.c_str(), outer.length()));
  ESP_ERROR_CHECK(esp_eap_client_set_username(
      (const uint8_t*)eapUsername.c_str(), eapUsername.length()));
  ESP_ERROR_CHECK(esp_eap_client_set_password(
      (const uint8_t*)eapPassword.c_str(), eapPassword.length()));

  // Ignore la validité temporelle du certificat (NTP pas encore sync)
  esp_eap_client_set_disable_time_check(true);

  // ── 5. Certificat CA (mode sécurisé uniquement) ────────────────────────
  if (!eapInsecure) {
    ESP_ERROR_CHECK(esp_eap_client_set_ca_cert(
      (const unsigned char*)_binary_src_certs_ca_rezoleo_pem_start,
      (int)(_binary_src_certs_ca_rezoleo_pem_end
            - _binary_src_certs_ca_rezoleo_pem_start)));
  }

  // ── 6. Activer Enterprise ──────────────────────────────────────────────
  esp_err_t er = esp_wifi_sta_enterprise_enable();
  if (er == ESP_ERR_WIFI_NOT_INIT) {
    WiFi.mode(WIFI_STA);
    er = esp_wifi_sta_enterprise_enable();
  }
  ESP_ERROR_CHECK(er);

  // ── 7. Lancer la connexion ─────────────────────────────────────────────
  LOGI("EAP", "Connexion à '%s'…", wifiSSID.c_str());
  provSet("status", "connecting");

  s_lastDiscReasonEap = -1;
  s_connectingEap     = true;   // autorise le handler à réagir dès maintenant

  WiFi.begin(wifiSSID.c_str());

  // ── 8. Attente ~20 s (80 × 250 ms) ────────────────────────────────────
  bool ok = false;
  for (int i = 0; i < 80; ++i) {
    if (WiFi.status() == WL_CONNECTED) { ok = true; break; }
    esp_task_wdt_reset();
    vTaskDelay(250 / portTICK_PERIOD_MS);
#if BREEZLY_LOG_LEVEL >= BREEZLY_LOG_LEVEL_DEBUG
    if ((i % 4) == 0) Serial.print(".");
#endif
  }
#if BREEZLY_LOG_LEVEL >= BREEZLY_LOG_LEVEL_DEBUG
  Serial.println();
#endif

  s_connectingEap = false;  // fin de la fenêtre de connexion

  // ── 9. Succès ──────────────────────────────────────────────────────────
  if (ok) {
    wifiConnected = true;
    breezly_on_wifi_ok();
    LOGI("EAP", "OK IP=%s RSSI=%d",
         WiFi.localIP().toString().c_str(), WiFi.RSSI());
    provSet("status", "wifi_ok");

    bool inet = checkInternetReachable();
    if (!inet) {
      LOGW("EAP", "Internet unreachable");
      provSet("status", "inet_unreachable");
      return false;
    }
    breezly_on_inet_ok();
    provSet("status", "inet_ok");
    provisioningNotifyConnected();
    startSNTPAfterConnected();

    // Note: ne PAS appeler esp_eap_client_clear_* ici.
    // Les buffers EAP sont nécessaires pour la reconnexion automatique.
    // Les libérer provoque des crashes MQTT après ~30-60 min d'uptime.

    return true;
  }

  // ── 10. Échec ──────────────────────────────────────────────────────────
  wifiConnected = false;
  LOGW("EAP", "Échec connexion, reason=%d", s_lastDiscReasonEap);
  provSet("status", mapDiscReasonToStatus(s_lastDiscReasonEap));

  if (s_lastDiscReasonEap == WIFI_REASON_802_1X_AUTH_FAILED) {
    breezly_on_wifi_auth_failed();
  } else if (s_lastDiscReasonEap == WIFI_REASON_NO_AP_FOUND  ||
             s_lastDiscReasonEap == WIFI_REASON_ASSOC_EXPIRE ||
             s_lastDiscReasonEap == WIFI_REASON_HANDSHAKE_TIMEOUT) {
    breezly_on_wifi_assoc_timeout();
  }

  if (bleInited) restartBLEAdvertising();
  return false;
}
