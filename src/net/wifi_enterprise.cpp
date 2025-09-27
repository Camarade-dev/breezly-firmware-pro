#include "wifi_enterprise.h"
#include "../core/globals.h"
#include "../net/sntp_utils.h"
#include <WiFi.h>

extern "C" {
  #include "esp_eap_client.h"   // API moderne (ESP-IDF 5.x)
  #include "esp_wifi.h"
}

// Certificat CA embarqué (via board_build.embed_txtfiles)
extern const uint8_t _binary_src_certs_ca_rezoleo_pem_start[];
extern const uint8_t _binary_src_certs_ca_rezoleo_pem_end[];

bool connectToWiFiEnterprise() {
  if (eapUsername.isEmpty() || eapPassword.isEmpty()) {
    Serial.println("[EAP] username/password manquants");
    return false;
  }
  if (wifiSSID.isEmpty()) {
    Serial.println("[EAP] SSID manquant");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);

  // Outer (anonymous) si fourni, sinon identity/username
  const String& outer = eapAnon.length() ? eapAnon
                         : eapIdentity.length() ? eapIdentity
                         : eapUsername;
  ESP_ERROR_CHECK( esp_eap_client_set_identity(
    (const unsigned char*)outer.c_str(), outer.length()) );

  // Inner (MSCHAPv2)
  const String& innerUser = eapUsername.length() ? eapUsername
                            : eapIdentity.length() ? eapIdentity
                            : outer;
  ESP_ERROR_CHECK( esp_eap_client_set_username(
    (const unsigned char*)innerUser.c_str(), innerUser.length()) );
  ESP_ERROR_CHECK( esp_eap_client_set_password(
    (const unsigned char*)eapPassword.c_str(), eapPassword.length()) );

  // CA obligatoire (anti-MITM)
    const uint8_t* ca_start = _binary_src_certs_ca_rezoleo_pem_start;
    const uint8_t* ca_end   = _binary_src_certs_ca_rezoleo_pem_end;
    ESP_ERROR_CHECK( esp_eap_client_set_ca_cert(
      (const unsigned char*)ca_start, (int)(ca_end - ca_start)) );

  // Active Enterprise (remplace wpa2_ent_enable)
  ESP_ERROR_CHECK( esp_wifi_sta_enterprise_enable() );

  Serial.printf("[EAP] Connexion à '%s'...\n", wifiSSID.c_str());
  WiFi.begin(wifiSSID.c_str());

  for (int i=0;i<60 && WiFi.status()!=WL_CONNECTED;i++){
    delay(500); Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.printf("[EAP] OK IP=%s\n", WiFi.localIP().toString().c_str());
    startSNTPAfterConnected();
    return true;
  } else {
    wifiConnected = false;
    Serial.println("[EAP] ÉCHEC");
    return false;
  }
}
