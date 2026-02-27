/**
 * MQTT control payload v1: validation, anti-replay, HMAC, rate limit.
 * Dispatches to mqtt_bus handlers after checks.
 */
#include "mqtt_ctrl.h"
#include "mqtt_bus.h"
#include "../core/log.h"
#include "../core/globals.h"
#include "../core/devkey_runtime.h"
#include "../app_config.h"
#include "../net/sntp_utils.h"
#include "../ota/ota.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <time.h>
#include <string.h>

// Reject reasons (log code only in prod, never full payload)
enum CtrlRejectReason {
  CTRL_REJECT_NONE = 0,
  CTRL_REJECT_EMPTY,
  CTRL_REJECT_JSON_PARSE,
  CTRL_REJECT_ACTION_UNKNOWN,
  CTRL_REJECT_MISSING_TS,
  CTRL_REJECT_MISSING_CMDID,
  CTRL_REJECT_TS_SKEW,
  CTRL_REJECT_REPLAY_CMDID,
  CTRL_REJECT_REPLAY_TS,
  CTRL_REJECT_RATE_LIMIT,
  CTRL_REJECT_MISSING_SIG,
  CTRL_REJECT_SIG_INVALID,
  CTRL_REJECT_SET_WIFI_TOO_LONG,
  CTRL_REJECT_SET_WIFI_OTA_BUSY,
  CTRL_REJECT_FACTORY_DISABLED,
  CTRL_REJECT_FACTORY_CONFIRM_HOLD,
};

static const char* rejectReasonStr(unsigned r) {
  switch (r) {
    case CTRL_REJECT_EMPTY: return "empty";
    case CTRL_REJECT_JSON_PARSE: return "json";
    case CTRL_REJECT_ACTION_UNKNOWN: return "action";
    case CTRL_REJECT_MISSING_TS: return "ts";
    case CTRL_REJECT_MISSING_CMDID: return "cmdId";
    case CTRL_REJECT_TS_SKEW: return "skew";
    case CTRL_REJECT_REPLAY_CMDID: return "replay_id";
    case CTRL_REJECT_REPLAY_TS: return "replay_ts";
    case CTRL_REJECT_RATE_LIMIT: return "rate";
    case CTRL_REJECT_MISSING_SIG: return "sig_missing";
    case CTRL_REJECT_SIG_INVALID: return "sig_invalid";
    case CTRL_REJECT_SET_WIFI_TOO_LONG: return "set_wifi_len";
    case CTRL_REJECT_SET_WIFI_OTA_BUSY: return "ota_busy";
    case CTRL_REJECT_FACTORY_DISABLED: return "factory_disabled";
    case CTRL_REJECT_FACTORY_CONFIRM_HOLD: return "factory_confirm";
    default: return "unknown";
  }
}

// Ring buffer: last accepted cmdIds (we store hash/fingerprint to save RAM: 8 bytes per slot)
#define CTRL_CMDID_FINGERPRINT_BYTES 8
static uint8_t s_cmdIdRing[CTRL_CMDID_RING_SIZE][CTRL_CMDID_FINGERPRINT_BYTES];
static uint8_t s_cmdIdRingHead;
static uint8_t s_cmdIdRingCount;
static time_t s_lastAcceptedTs;
static uint32_t s_rateLimitTokens;
static uint32_t s_rateLimitLastRefillMs;

static void fingerprintCmdId(const char* cmdId, size_t len, uint8_t out[CTRL_CMDID_FINGERPRINT_BYTES]) {
  // Simple FNV-1a style fingerprint (no crypto needed, just dedup)
  uint32_t h1 = 2166136261u, h2 = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    uint8_t c = (uint8_t)cmdId[i];
    h1 ^= c; h1 *= 16777619u;
    h2 ^= (uint8_t)(c + 7); h2 *= 16777619u;
  }
  memcpy(out, &h1, 4);
  memcpy(out + 4, &h2, 4);
}

static bool cmdIdSeen(const char* cmdId, size_t len) {
  if (len == 0) return true;
  uint8_t fp[CTRL_CMDID_FINGERPRINT_BYTES];
  fingerprintCmdId(cmdId, len, fp);
  for (uint8_t i = 0; i < s_cmdIdRingCount; i++) {
    if (memcmp(s_cmdIdRing[i], fp, CTRL_CMDID_FINGERPRINT_BYTES) == 0)
      return true;
  }
  return false;
}

static void cmdIdRingPush(const char* cmdId, size_t len) {
  if (len == 0) return;
  uint8_t fp[CTRL_CMDID_FINGERPRINT_BYTES];
  fingerprintCmdId(cmdId, len, fp);
  memcpy(s_cmdIdRing[s_cmdIdRingHead], fp, CTRL_CMDID_FINGERPRINT_BYTES);
  s_cmdIdRingHead = (s_cmdIdRingHead + 1) % CTRL_CMDID_RING_SIZE;
  if (s_cmdIdRingCount < CTRL_CMDID_RING_SIZE) s_cmdIdRingCount++;
}

static bool constantTimeEqual(const char* a, const char* b, size_t len) {
  if (!a || !b) return false;
  volatile uint8_t diff = 0;
  for (size_t i = 0; i < len; i++) {
    diff |= (uint8_t)(a[i] ^ b[i]);
  }
  return diff == 0;
}

static bool verifyHmac(const char* canonical, size_t canonLen, const char* sigB64, const uint8_t* key, size_t keyLen) {
  uint8_t mac[32];
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) return false;
  if (mbedtls_md_hmac(info, key, keyLen, (const unsigned char*)canonical, canonLen, mac) != 0)
    return false;
  size_t olen;
  unsigned char dec[64];
  if (mbedtls_base64_decode(dec, sizeof(dec), &olen, (const unsigned char*)sigB64, strlen(sigB64)) != 0)
    return false;
  if (olen != 32) return false;
  return constantTimeEqual((const char*)mac, (const char*)dec, 32);
}

static void buildActionArgsCanonical(const char* action, const JsonDocument& j, String& out) {
  out = "";
  if (strcmp(action, "set_wifi") == 0) {
    const char* ssid = j["ssid"] | "";
    const char* pwd  = j["password"] | "";
    int eap = (strcmp(j["authType"] | "psk", "eap") == 0) ? 1 : 0;
    const char* user = eap ? (j["eap"]["username"] | "") : "";
    out = "ssid=" + String(ssid) + "|eap=" + String(eap) + "|user=" + String(user) + "|pw=" + String(pwd);
  } else if (strcmp(action, "update") == 0) {
    const char* target = j["target"] | "";
    int force = j["force"] | 0;
    out = "target=" + String(target) + "|force=" + String(force);
  } else if (strcmp(action, "set_night_mode") == 0) {
    const char* mode = j["mode"] | "auto";
    int enabled = (strcmp(mode, "on") == 0) ? 1 : 0;
    out = "enabled=" + String(enabled);
  } else if (strcmp(action, "forget_wifi") == 0) {
    out = "";
  } else if (strcmp(action, "factory_reset") == 0) {
    bool confirm = j["confirm"] | false;
    int holdMs = j["holdMs"] | 0;
    out = "confirm=" + String(confirm ? 1 : 0) + "|holdMs=" + String(holdMs);
  }
}

static void buildCanonical(const char* action, int64_t ts, const char* cmdId, const JsonDocument& j, String& canonical) {
  String args;
  buildActionArgsCanonical(action, j, args);
  canonical = "v1|" + sensorId + "|" + String(action) + "|" + String((long)ts) + "|" + String(cmdId) + "|" + args;
}

void mqttCtrlHandleMessage(const char* topic, const uint8_t* payload, unsigned int len) {
  if (!topic || strcmp(topic, mqtt_topic_ctrl().c_str()) != 0) return;
  if (len == 0) return;

  if (len > CTRL_PAYLOAD_MAX_BYTES) {
    LOGW("CTRL", "reject payload_too_long");
    mqtt_bus_publish_control_ack("ctrl", false, "payload_too_long");
    return;
  }

  String msg;
  msg.reserve(len + 1);
  for (unsigned i = 0; i < len; i++) msg += (char)payload[i];

  StaticJsonDocument<CTRL_PAYLOAD_MAX_BYTES> doc;
  DeserializationError err = deserializeJson(doc, msg);
  if (err) {
    LOGW("CTRL", "reject json");
    mqtt_bus_publish_control_ack("ctrl", false, rejectReasonStr(CTRL_REJECT_JSON_PARSE));
    return;
  }

  const char* action = doc["action"] | "";
  if (!action[0]) {
    LOGW("CTRL", "reject action");
    mqtt_bus_publish_control_ack("ctrl", false, rejectReasonStr(CTRL_REJECT_ACTION_UNKNOWN));
    return;
  }

  // Whitelist
  if (strcmp(action, "set_wifi") != 0 && strcmp(action, "update") != 0 &&
      strcmp(action, "set_night_mode") != 0 && strcmp(action, "forget_wifi") != 0 &&
      strcmp(action, "factory_reset") != 0) {
    LOGW("CTRL", "reject action");
    mqtt_bus_publish_control_ack("ctrl", false, rejectReasonStr(CTRL_REJECT_ACTION_UNKNOWN));
    return;
  }

  int64_t ts = doc["ts"] | 0LL;
  const char* cmdId = doc["cmdId"] | "";
  const char* sig = doc["sig"] | "";

#if CTRL_REQUIRE_SIG
  bool requireSig = true;
#else
  bool requireSig = (CTRL_ALLOW_UNSIGNED == 0);
#endif
  if (requireSig) {
    if (!ts) {
      LOGW("CTRL", "reject ts");
      mqtt_bus_publish_control_ack(action, false, rejectReasonStr(CTRL_REJECT_MISSING_TS));
      return;
    }
    if (!cmdId[0]) {
      LOGW("CTRL", "reject cmdId");
      mqtt_bus_publish_control_ack(action, false, rejectReasonStr(CTRL_REJECT_MISSING_CMDID));
      return;
    }
    if (!sig[0]) {
      LOGW("CTRL", "reject sig_missing");
      mqtt_bus_publish_control_ack(action, false, rejectReasonStr(CTRL_REJECT_MISSING_SIG));
      return;
    }
  }

  time_t now = time(nullptr);
  // Skew check: skip for forget_wifi (device clock may be wrong or NTP not yet synced after boot)
  if (strcmp(action, "forget_wifi") != 0) {
    if (requireSig && timeIsSane()) {
      int64_t skew = (int64_t)now - ts;
      if (skew < 0) skew = -skew;
      if (skew > (int64_t)CTRL_MAX_SKEW_SEC) {
        LOGW("CTRL", "reject skew");
        mqtt_bus_publish_control_ack(action, false, rejectReasonStr(CTRL_REJECT_TS_SKEW));
        return;
      }
    }
  }

  if (cmdId[0]) {
    if (cmdIdSeen(cmdId, strlen(cmdId))) {
      LOGW("CTRL", "reject replay_id");
      mqtt_bus_publish_control_ack(action, false, rejectReasonStr(CTRL_REJECT_REPLAY_CMDID));
      return;
    }
    if (s_cmdIdRingCount > 0 && ts > 0 && (int64_t)s_lastAcceptedTs > 0 && ts <= (int64_t)s_lastAcceptedTs - 1) {
      LOGW("CTRL", "reject replay_ts");
      mqtt_bus_publish_control_ack(action, false, rejectReasonStr(CTRL_REJECT_REPLAY_TS));
      return;
    }
  }

  // Rate limit (forget_wifi exempt: must always be accepted when user deletes sensor)
  if (strcmp(action, "forget_wifi") != 0) {
    uint32_t nowMs = (uint32_t)millis();
    if (s_rateLimitLastRefillMs == 0) s_rateLimitLastRefillMs = nowMs;
    uint32_t elapsed = nowMs - s_rateLimitLastRefillMs;
    if (elapsed >= CTRL_RATE_LIMIT_MIN_MS) {
      uint32_t refill = elapsed / CTRL_RATE_LIMIT_MIN_MS;
      s_rateLimitTokens = (s_rateLimitTokens + refill) > CTRL_RATE_LIMIT_BURST ? CTRL_RATE_LIMIT_BURST : (s_rateLimitTokens + refill);
      s_rateLimitLastRefillMs = nowMs;
    }
    if (s_rateLimitTokens == 0) {
      LOGW("CTRL", "reject rate");
      mqtt_bus_publish_control_ack(action, false, rejectReasonStr(CTRL_REJECT_RATE_LIMIT));
      return;
    }
    s_rateLimitTokens--;
  }

  if (requireSig && sig[0]) {
    if (g_deviceKeyB64.length() < 4) {
      LOGW("CTRL", "reject no_key");
      mqtt_bus_publish_control_ack(action, false, "no_key");
      return;
    }
    size_t keyLen;
    unsigned char keyBuf[64];
    if (mbedtls_base64_decode(keyBuf, sizeof(keyBuf), &keyLen, (const unsigned char*)g_deviceKeyB64.c_str(), g_deviceKeyB64.length()) != 0) {
      LOGW("CTRL", "reject key_decode");
      mqtt_bus_publish_control_ack(action, false, "key_err");
      return;
    }
    String canonical;
    buildCanonical(action, ts, cmdId, doc, canonical);
    if (!verifyHmac(canonical.c_str(), canonical.length(), sig, keyBuf, keyLen)) {
      LOGW("CTRL", "reject sig_invalid");
      mqtt_bus_publish_control_ack(action, false, rejectReasonStr(CTRL_REJECT_SIG_INVALID));
      return;
    }
  }

  // Guards
  if (strcmp(action, "set_wifi") == 0) {
    size_t ssidLen = strlen(doc["ssid"] | "");
    size_t pwdLen = strlen(doc["password"] | "");
    if (ssidLen > CTRL_SET_WIFI_SSID_MAX || pwdLen > CTRL_SET_WIFI_PASSWORD_MAX) {
      LOGW("CTRL", "reject set_wifi_len");
      mqtt_bus_publish_control_ack(action, false, rejectReasonStr(CTRL_REJECT_SET_WIFI_TOO_LONG));
      return;
    }
    if (otaIsInProgress()) {
      LOGW("CTRL", "reject ota_busy");
      mqtt_bus_publish_control_ack(action, false, rejectReasonStr(CTRL_REJECT_SET_WIFI_OTA_BUSY));
      return;
    }
  }

  if (strcmp(action, "factory_reset") == 0) {
#if !CTRL_FACTORY_RESET_ENABLED
    LOGW("CTRL", "reject factory_disabled");
    mqtt_bus_publish_control_ack(action, false, rejectReasonStr(CTRL_REJECT_FACTORY_DISABLED));
    return;
#else
    bool confirm = doc["confirm"] | false;
    int holdMs = doc["holdMs"] | 0;
    if (!confirm || holdMs < (int)CTRL_FACTORY_RESET_REQUIRE_HOLD_MS) {
      LOGW("CTRL", "reject factory_confirm");
      mqtt_bus_publish_control_ack(action, false, rejectReasonStr(CTRL_REJECT_FACTORY_CONFIRM_HOLD));
      return;
    }
#endif
  }

  // Accept: record cmdId and ts
  if (cmdId[0]) cmdIdRingPush(cmdId, strlen(cmdId));
  s_lastAcceptedTs = (ts > 0) ? (time_t)ts : now;

  // Dispatch
  if (strcmp(action, "set_wifi") == 0) {
    mqtt_bus_handle_set_wifi(doc);
    return;
  }
  if (strcmp(action, "update") == 0) {
    mqtt_bus_handle_update();
    return;
  }
  if (strcmp(action, "set_night_mode") == 0) {
    mqtt_bus_handle_set_night_mode(doc);
    return;
  }
  if (strcmp(action, "forget_wifi") == 0) {
    mqtt_bus_handle_forget_wifi(doc);
    return;
  }
  if (strcmp(action, "factory_reset") == 0) {
    mqtt_bus_handle_factory_reset(doc);
    return;
  }
}
