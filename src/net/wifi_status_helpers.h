#pragma once
#include <WiFi.h>
#include "../ble/provisioning.h"

static inline void provSet(const char* key, const char* val) {
  char buf[96];
  snprintf(buf, sizeof(buf), "{\"%s\":\"%s\"}", key, val);
  provisioningSetStatus(buf);
}

// Test Internet sans dépendre de l'heure: d'abord TCP sur IP directe, puis DNS+TCP.
static inline bool checkInternetReachable(uint32_t dnsTimeoutMs = 3000, uint32_t tcpTimeoutMs = 2500) {
  WiFiClient c;
  c.setTimeout((tcpTimeoutMs/1000) + 1);

  // 1) Pas de DNS (insensible à l'heure)
  if (c.connect(IPAddress(1,1,1,1), 80)) { c.stop(); return true; }
  if (c.connect(IPAddress(8,8,8,8), 80)) { c.stop(); return true; }

  // 2) DNS simple (toujours insensible à l'heure), puis TCP 80
  IPAddress ip;
  unsigned long t0 = millis();
  if (WiFi.hostByName("one.one.one.one", ip) == 1) {
    if ((millis() - t0) <= dnsTimeoutMs) {
      if (c.connect(ip, 80)) { c.stop(); return true; }
    }
  }
  return false;
}

// Mapping raison→statut lisible
static inline const char* mapDiscReasonToStatus(int reason) {
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
