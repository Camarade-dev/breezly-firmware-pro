// wifi_status_helpers.h (par ex.)
#pragma once
#include <WiFi.h>
#include "../ble/provisioning.h"

static inline void provSet(const char* key, const char* val) {
  // JSON minimal, évite ArduinoJson ici pour rester léger
  char buf[96];
  snprintf(buf, sizeof(buf), "{\"%s\":\"%s\"}", key, val);
  provisioningSetStatus(buf);
}

// Test Internet simple: DNS + TCP courte durée
static inline bool checkInternetReachable(uint32_t dnsTimeoutMs = 3000, uint32_t tcpTimeoutMs = 2500) {
  IPAddress ip;
  uint32_t t0 = millis();
  if (WiFi.hostByName("one.one.one.one", ip) != 1) return false; // DNS KO
  if (millis() - t0 > dnsTimeoutMs) return false;

  WiFiClient c;
  c.setTimeout(tcpTimeoutMs / 1000);
  if (!c.connect(ip, 80)) return false; // TCP KO
  c.stop();
  return true;
}

// Mapping raison→statut d'échec lisible
static inline const char* mapDiscReasonToStatus(int reason) {
  // cf. wifi_err_reason_t (ESP-IDF): https://docs.espressif.com/
  switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return "wifi_auth_failed";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_ASSOC_EXPIRE:
    case WIFI_REASON_NOT_AUTHED:
    case WIFI_REASON_NOT_ASSOCED:
    case WIFI_REASON_AP_TSF_RESET:
    case WIFI_REASON_NO_AP_FOUND:
      return "wifi_assoc_timeout";
    default:
      return "error";
  }
}
