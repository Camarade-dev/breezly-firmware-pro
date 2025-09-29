#include "wifi_enterprise.h"
#include "../core/globals.h"
#include "../net/sntp_utils.h"

#include <WiFi.h>
#include "esp_task_wdt.h"

extern "C" {
  #include "esp_wifi.h"
  #include "esp_event.h"
  #include "esp_eap_client.h"   // API Enterprise moderne (ESP-IDF 5.x)
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
  // Champs requis EAP
  if (wifiSSID.isEmpty() || eapUsername.isEmpty() || eapPassword.isEmpty()) {
    Serial.println("[EAP] champs manquants (ssid/user/pass)");
    return false;
  }

  // Préparer WiFi via Arduino (NE PAS étendre la pile → wifioff=false)
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, true);           // efface juste l'AP/état
  esp_wifi_set_ps(WIFI_PS_NONE);          // pas d’économie d’énergie pendant l’auth

  // Handler de déconnexion (diag, enregistré 1 seule fois)
  if (!s_discInst) {
    esp_event_handler_instance_register(
      WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &onStaDisc, nullptr, &s_discInst);
  }

  // Nettoyage Enterprise "best effort"
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

  // Identités + MDP + CA
  const String outer = eapAnon.length() ? eapAnon : "ano@rezoleo.fr";
  ESP_ERROR_CHECK( esp_eap_client_set_identity((const uint8_t*)outer.c_str(), outer.length()) );
  ESP_ERROR_CHECK( esp_eap_client_set_username((const uint8_t*)eapUsername.c_str(), eapUsername.length()) );
  ESP_ERROR_CHECK( esp_eap_client_set_password((const uint8_t*)eapPassword.c_str(), eapPassword.length()) );

  // Si l’horloge n’est pas à l’heure au boot → évite l’échec « cert non encore valide »
  esp_eap_client_set_disable_time_check(true);

  const uint8_t* ca_start = _binary_src_certs_ca_rezoleo_pem_start;
  const uint8_t* ca_end   = _binary_src_certs_ca_rezoleo_pem_end;
  ESP_ERROR_CHECK( esp_eap_client_set_ca_cert(
      (const unsigned char*)ca_start, (int)(ca_end - ca_start)) );

  // Activer EAP (si NOT_INIT, s’assurer du mode STA et réessayer)
  esp_err_t er = esp_wifi_sta_enterprise_enable();
  if (er == ESP_ERR_WIFI_NOT_INIT) {
    WiFi.mode(WIFI_STA);
    er = esp_wifi_sta_enterprise_enable();
  }
  ESP_ERROR_CHECK(er);

  // Associer via Arduino (ne pas utiliser esp_wifi_connect ici)
  Serial.printf("[EAP] Connexion à '%s'…\n", wifiSSID.c_str());
  pauseOtherNetWork();
  WiFi.begin(wifiSSID.c_str());

  // Attente ~20 s avec feed WDT
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
    Serial.printf("[EAP] OK IP=%s  RSSI=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());

    // Démarre SNTP APRÈS le lien Wi-Fi (évite les asserts lwIP)
    startSNTPAfterConnected();

    // (Optionnel) tu pourras réactiver la vérif de date plus tard si tu veux :
    // esp_eap_client_set_disable_time_check(false);

    resumeOtherNetWork();
    return true;
  } else {
    wifiConnected = false;
    Serial.println("[EAP] ÉCHEC");
    resumeOtherNetWork();
    return false;
  }
}
