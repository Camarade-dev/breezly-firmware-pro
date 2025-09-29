#include "wifi_enterprise.h"
#include "../core/globals.h"
#include "../net/sntp_utils.h"
#include <WiFi.h>
#include "esp_task_wdt.h"

extern "C" {
  #include "esp_eap_client.h"   // API moderne (ESP-IDF 5.x)
  #include "esp_wifi.h"
}
// --- Logs de déconnexion STA pour diagnostiquer les échecs EAP ---
static void onStaDisc(void*, esp_event_base_t, int32_t, void* data){
  auto* ev = (wifi_event_sta_disconnected_t*)data;
  Serial.printf("[EAP] STA_DISCONNECTED reason=%d\n", ev->reason);
}

// Certificat CA embarqué (via board_build.embed_txtfiles)
extern const uint8_t _binary_src_certs_ca_rezoleo_pem_start[];
extern const uint8_t _binary_src_certs_ca_rezoleo_pem_end[];
static bool ensureWifiInit() {
  static bool inited = false;
  if (inited) return true;

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_t* netif = esp_netif_create_default_wifi_sta();
  (void)netif;

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_err_t e = esp_wifi_init(&cfg);
  if (e != ESP_OK && e != ESP_ERR_WIFI_INIT_STATE) return false;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());

  // Enregistre le handler de déconnexion
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &onStaDisc, nullptr, nullptr));

  inited = true;
  return true;
}
bool connectToWiFiEnterprise() {
  if (eapUsername.isEmpty() || eapPassword.isEmpty() || wifiSSID.isEmpty()) {
    Serial.println("[EAP] champs manquants");
    return false;
  }

  // 0) Stack Arduino WiFi, clean start – comme dans le minimal
  WiFi.mode(WIFI_STA);
  // (optionnel) fixe un hostname si tu veux
  // WiFi.setHostname("breezly-esp32");
  WiFi.disconnect(true, true);     // reset complet (STA pas encore “starté”)

  // 1) Outer/inner + password (même ordre que le sketch minimal)
  const String outer = eapAnon.length() ? eapAnon : "ano@rezoleo.fr";
  ESP_ERROR_CHECK( esp_eap_client_set_identity(
      (const uint8_t*)outer.c_str(), outer.length()) );
  ESP_ERROR_CHECK( esp_eap_client_set_username(
      (const uint8_t*)eapUsername.c_str(), eapUsername.length()) );
  ESP_ERROR_CHECK( esp_eap_client_set_password(
      (const uint8_t*)eapPassword.c_str(), eapPassword.length()) );

  // 2) Certificat CA embarqué (comme ton minimal “avec CA”)
  extern const uint8_t _binary_src_certs_ca_rezoleo_pem_start[];
  extern const uint8_t _binary_src_certs_ca_rezoleo_pem_end[];
  const uint8_t* ca_start = _binary_src_certs_ca_rezoleo_pem_start;
  const uint8_t* ca_end   = _binary_src_certs_ca_rezoleo_pem_end;
  ESP_ERROR_CHECK( esp_eap_client_set_ca_cert(
      (const unsigned char*)ca_start, (int)(ca_end - ca_start)) );

  // ⚠️ Pour un test diag, tu peux temporairement désactiver la vérification :
  // esp_eap_client_set_disable_cert_check(true);

  // 3) Active le supplicant Enterprise (remplace wpa2_ent_enable)
  ESP_ERROR_CHECK( esp_wifi_sta_enterprise_enable() );

  // 4) Associe via l’API Arduino (pas esp_wifi_set_config)
  Serial.printf("[EAP] Connexion à '%s'…\n", wifiSSID.c_str());
  WiFi.begin(wifiSSID.c_str());

  // 5) Attente (feed WDT), exactement comme ton minimal
  for (int i = 0; i < 80 && WiFi.status() != WL_CONNECTED; ++i) {
    esp_task_wdt_reset();
    vTaskDelay(250 / portTICK_PERIOD_MS);
    if ((i % 2) == 0) Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.printf("[EAP] OK IP=%s  RSSI=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    // SNTP APRÈS connexion (comme dans le minimal)
    startSNTPAfterConnected();
    // (optionnel) log raison de déconn si un jour ça coupe :
    // esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, onStaDisc, nullptr, nullptr);
    return true;
  } else {
    wifiConnected = false;
    Serial.println("[EAP] ÉCHEC");
    return false;
  }
}


