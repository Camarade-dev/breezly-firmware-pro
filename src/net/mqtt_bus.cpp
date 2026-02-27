#include "mqtt_bus.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "../ble/provisioning.h"
#include <freertos/event_groups.h>
#include <Preferences.h>
#include "../core/globals.h"     // wifiConnected, sensorId, userId, prefs, CURRENT_FIRMWARE_VERSION
#include "../core/backoff.h"
#include "../led/led_status.h"
#include "../ota/ota.h"
#include "../net/sntp_utils.h"
#include <string>
#include "../ca_bundle.h"
#include <esp_system.h>

#include "../app_config.h"
#include "mqtt_secrets.h"
#include <esp_ota_ops.h>
#if defined(BREEZLY_PROD)
static const char* MQTT_PREFIX = "prod/";
#elif defined(BREEZLY_DEV)
static const char* MQTT_PREFIX = "dev/";
#else
static const char* MQTT_PREFIX = "dev/";
#endif
// ======= PARAMS BROKER (user/pass depuis mqtt_secrets.h, généré par pre-build) =======
static const char* MQTT_HOST = "607207c4394d44b8bad11a33e8ed591d.s1.eu.hivemq.cloud";
static const int   MQTT_PORT = 8883;
static const char* MQTT_USER = MQTT_SECRET_USER;
static const char* MQTT_PASS = MQTT_SECRET_PASS;

// ======= Ressources de la tâche MQTT =======
static WiFiClientSecure s_tls;
static PubSubClient     s_mqtt(s_tls);
static QueueHandle_t    s_queue = nullptr;
static EventGroupHandle_t s_ev   = nullptr;

static volatile bool s_connected = false;
static volatile bool s_ready     = false;

static const int EV_REQ_CONNECT_BIT = (1 << 0);
static uint32_t s_lastConnAttemptMs = 0;

static const BackoffConfig s_mqttBackoffConfig = {
  BACKOFF_MQTT_MIN_MS,
  BACKOFF_MQTT_MAX_MS,
  BACKOFF_MQTT_FACTOR,
  BACKOFF_MQTT_JITTER_PERCENT
};
static Backoff s_mqttBackoff(s_mqttBackoffConfig);

static String withPrefix(const String& t) {
  if (t.startsWith("dev/") || t.startsWith("prod/")) return t;
  return String(MQTT_PREFIX) + t;
}
// ======= Topics helpers =======
String mqtt_topic_device_base() { return withPrefix("breezly/devices/" + sensorId); }
String mqtt_topic_ota()    { return mqtt_topic_device_base() + "/ota"; }
String mqtt_topic_ctrl()   { return mqtt_topic_device_base() + "/control"; }
String mqtt_topic_status() { return mqtt_topic_device_base() + "/status"; }
String mqtt_topic_telemetry() { return mqtt_topic_device_base() + "/telemetry"; }

bool mqtt_telemetry_emit(const char* type, const char* context_json) {
  if (!type) return false;
  DynamicJsonDocument doc(512);
  doc["type"] = type;
  doc["fw_version"] = CURRENT_FIRMWARE_VERSION;
  doc["device_id"] = sensorId;
  doc["ts"] = (uint64_t)(time(nullptr) * 1000ULL);
  if (context_json && context_json[0]) {
    StaticJsonDocument<256> ctx;
    if (deserializeJson(ctx, context_json) == DeserializationError::Ok)
      doc["context"] = ctx.as<JsonObject>();
  }
  String payload;
  serializeJson(doc, payload);
  return mqtt_enqueue(mqtt_topic_telemetry(), payload, 0, false);
}
static volatile bool s_hello_ok      = false;
static volatile bool s_registered_ok = false;

static void maybe_fire_connected_final() {
  if (s_hello_ok && s_registered_ok) {
    breezly_on_connected_final();   // ← étape “tout est prêt” pour l’app
  }
}
// --- Helpers unpair / reboot ---
static void forgetWifiPrefs_All(bool alsoForgetUser){
  Preferences p;
  if (p.begin("myApp", false)) {
    p.putUInt("wifiAuthType", (uint32_t)WIFI_CONN_PSK);
    p.putString("wifiSSID", "");
    p.putString("wifiPassword", "");
    p.putString("eapUsername", "");
    p.putString("eapPassword", "");
    p.putString("eapIdentity", "");
    p.putString("eapAnon", "");
    if (alsoForgetUser) {
      p.putString("userId", "");
      // tu peux aussi vider "sensorId" si tu veux forcer un re-claim complet
      // p.putString("sensorId", "");
    }
    p.end();
  }

  // Recharge globals en RAM
  prefs.begin("myApp", true);
  wifiSSID     = prefs.getString("wifiSSID", "");
  wifiPassword = prefs.getString("wifiPassword", "");
  wifiAuthType = (WifiAuthType)prefs.getUInt("wifiAuthType",(uint32_t)WIFI_CONN_PSK);
  eapIdentity  = prefs.getString("eapIdentity", "");
  eapUsername  = prefs.getString("eapUsername", "");
  eapPassword  = prefs.getString("eapPassword", "");
  eapAnon      = prefs.getString("eapAnon", "");
  if (alsoForgetUser) {
    userId = prefs.getString("userId", "");
  }
  prefs.end();
}

static void gracefulUnpairAndReboot(const char* reason){
  // 1) Annonce retained “erasing/unpaired”
  DynamicJsonDocument st(160);
  st["state"]  = "erasing";
  st["reason"] = reason ? reason : "unpair";
  st["ts"]     = (uint64_t)(time(nullptr) * 1000ULL);
  String s; serializeJson(st, s);
  s_mqtt.publish(mqtt_topic_status().c_str(), s.c_str(), true); // retained

  // 2) Clear retained /control pour éviter re-appliquer après reboot
  String ctrlTopic = mqtt_topic_ctrl();
  s_mqtt.publish(ctrlTopic.c_str(), "", true);
  s_mqtt.loop();
  delay(100);

  // 3) Couper MQTT+WiFi proprement
  s_mqtt.disconnect();
  WiFi.disconnect(true, true);

  // 4) Feedback LED et reboot
  updateLedState(LED_UPDATING);
  delay(150);
  ESP.restart();
}

// ======= API =======
bool mqtt_is_connected() { return s_connected; }

bool mqtt_enqueue(const String& t, const String& p, uint8_t qos, bool retain) {
  
  if (!s_queue) return false;
  // 🧩 Applique le préfixe MQTT (ex: "dev/") uniquement si le topic n'en a pas déjà un
  String topicFull;
  if (t.startsWith("breezly/") || t.startsWith("dev/") || t.startsWith("prod/")) {
    topicFull = t;
  } else {
    topicFull = String(MQTT_PREFIX) + t;
  }
  PubMsg m{
    strdup(topicFull.c_str()),
    strdup(p.c_str()),
    qos,
    retain
  };
  Serial.printf("[MQTT] Enqueue vers topic: %s\n", topicFull.c_str());
  Serial.printf("[MQTT] sub ctrl=%s\n", mqtt_topic_ctrl().c_str());
  if (!m.topic || !m.payload) {
    if (m.topic) free(m.topic);
    if (m.payload) free(m.payload);
    return false;
  }

  return xQueueSend(s_queue, &m, 0) == pdTRUE;
}



void mqtt_request_connect() {
  if (s_ev) xEventGroupSetBits(s_ev, EV_REQ_CONNECT_BIT);
}

bool mqtt_flush(uint32_t timeout_ms) {
  if (!s_queue) return true;
  uint32_t t0 = millis();
  for (;;) {
    if (uxQueueMessagesWaiting(s_queue) == 0) return true;
    if ((millis() - t0) >= timeout_ms) return false;
    vTaskDelay(5 / portTICK_PERIOD_MS);
  }
}

// ======= Helpers internes =======
static void publish_status_retained(const char* state, const char* reason=nullptr) {
  DynamicJsonDocument j(160);
  j["state"] = state;
  j["ts"]    = (uint64_t)(time(nullptr) * 1000ULL);
  if (reason) j["reason"] = reason;
  String s; serializeJson(j, s);
  s_mqtt.publish(mqtt_topic_status().c_str(), s.c_str(), true); // retained
}

static bool applyWifiPrefsFromJson(const JsonDocument& j, String& errMsg) {
  Preferences p;
  if (!p.begin("myApp", false)) { errMsg = "prefs_begin_failed"; return false; }

  const char* auth = j["authType"] | "psk";
  WifiAuthType newAuth = (strcmp(auth,"eap")==0) ? WIFI_CONN_EAP_PEAP_MSCHAPV2 : WIFI_CONN_PSK;

  const char* ssid = j["ssid"] | "";
  const char* pwd  = j["password"] | "";

  const char* eapUser = j["eap"]["username"] | "";
  const char* eapPass = j["eap"]["password"] | "";
  const char* eapId   = j["eap"]["identity"] | "";
  const char* eapAnon = j["eap"]["anon"] | "";

  if (newAuth == WIFI_CONN_PSK) {
    if (!ssid[0] || !pwd[0]) { errMsg="missing_ssid_or_pwd"; p.end(); return false; }
  } else {
    if (!ssid[0] || !eapUser[0] || !eapPass[0]) { errMsg="missing_eap_fields"; p.end(); return false; }
  }

  p.putUInt("wifiAuthType", (uint32_t)newAuth);
  p.putString("wifiSSID",     ssid ? ssid : "");
  p.putString("wifiPassword", pwd  ? pwd  : "");

  if (newAuth == WIFI_CONN_EAP_PEAP_MSCHAPV2) {
    p.putString("eapUsername", eapUser ? eapUser : "");
    p.putString("eapPassword", eapPass ? eapPass : "");
    p.putString("eapIdentity", eapId   ? eapId   : "");
    p.putString("eapAnon",     eapAnon ? eapAnon : "anon@domain");
  } else {
    p.putString("eapUsername", "");
    p.putString("eapPassword", "");
    p.putString("eapIdentity", "");
    p.putString("eapAnon",     "");
  }
  p.end();
  return true;
}

static void publish_control_ack(const char* type, bool ok, const char* reason=nullptr){
  DynamicJsonDocument j(160);
  j["ack"]   = type;
  j["ok"]    = ok;
  j["ts"]    = (uint64_t)(time(nullptr) * 1000ULL);
  if (reason) j["reason"] = reason;
  String s; serializeJson(j, s);
  s_mqtt.publish(mqtt_topic_status().c_str(), s.c_str(), false);
}

static void handleSetWifi(const JsonDocument& j){
  String err;
  bool ok = applyWifiPrefsFromJson(j, err);
  publish_control_ack("set_wifi", ok, ok ? nullptr : err.c_str());
  if (!ok) return;

  // Recharge globals
  prefs.begin("myApp", true);
  wifiSSID     = prefs.getString("wifiSSID", "");
  wifiPassword = prefs.getString("wifiPassword", "");
  wifiAuthType = (WifiAuthType)prefs.getUInt("wifiAuthType",(uint32_t)WIFI_CONN_PSK);
  eapIdentity  = prefs.getString("eapIdentity", "");
  eapUsername  = prefs.getString("eapUsername", "");
  eapPassword  = prefs.getString("eapPassword", "");
  eapAnon      = prefs.getString("eapAnon", "");
  prefs.end();

  // Efface le retained sur /control AVANT coupure Wi-Fi
  String ctrlTopic = mqtt_topic_ctrl();
  s_mqtt.publish(ctrlTopic.c_str(), "", true);  // clear retained
  s_mqtt.loop();
  delay(50);

  // Laisse la couche Wi-Fi se reconnecter via ton flux existant
  WiFi.disconnect(true, true);
  wifiConnected     = false;
  needToConnectWiFi = true;
  updateLedState(LED_BOOT);
}

// ======= Callback message (CTRL/OTA) =======
static void onMqttMessage(char* topic, uint8_t* payload, unsigned int len) {
  String t(topic);
  if (len == 0) return; // retained cleared

  String msg; msg.reserve(len+1);
  for (unsigned i=0;i<len;i++) msg += (char)payload[i];
  if (t == mqtt_topic_status()) {
    StaticJsonDocument<384> j;
    auto err = deserializeJson(j, msg);
    if (err) return;

    // on standardise 2 formes:
    //  - {"state":"registered", ...}
    //  - {"registered":true, ...}
    const char* st = j["state"] | "";
    bool reg = j["registered"] | false;
    if ((st && strcmp(st, "registered")==0) || reg) {
      s_registered_ok = true;
      breezly_on_registered();
      maybe_fire_connected_final();
    }
    return;
  }

  if (t == mqtt_topic_ctrl()) {
    
    StaticJsonDocument<512> j;  
    auto err = deserializeJson(j, msg);
    if (err) { Serial.printf("[MQTT] JSON parse error: %s\n", err.c_str()); return; }
    const char* action = j["action"] | "";
    
    if (!action[0]) return;

    if (strcmp(action, "set_wifi")==0) { handleSetWifi(j); return; }

    if (strcmp(action, "update")==0) {
        Serial.println("[OTA] Trigger via MQTT");
        otaSetInProgress(true); // ← pause immédiate de la tâche MQTT
        xTaskCreatePinnedToCore([](void*){
        vTaskDelay(100/portTICK_PERIOD_MS);
        checkAndPerformCloudOTA();
        otaSetInProgress(false);
        vTaskDelete(NULL);
      }, "OTA_TASK", 8192, NULL, 1, NULL, 0);
      return;
    }
    if (strcmp(action, "set_night_mode")==0) {
      const char* mode = j["mode"] | "auto";
      int v = 0;
      if (strcmp(mode, "on")==0) v = 1;
      else if (strcmp(mode, "off")==0) v = 2;
      ledSetNightModeOverride(v);
      publish_control_ack("set_night_mode", true);
      return;
    }
    if (strcmp(action, "forget_wifi")==0) {
      // —— Idempotence guard
      uint64_t cmdTs = j["ts"] | 0ULL;
      const char* cmdId = j["cmdId"] | "";

      Preferences pGuard;
      uint64_t lastTs = 0;
      String   lastId = "";
      if (pGuard.begin("myApp", true)) {
        lastTs = pGuard.getULong64("forget_ts", 0ULL);
        lastId = pGuard.getString("forget_id", "");
        pGuard.end();
      }

      // If same id or older/equal timestamp, ignore (stale retained)
      if ((cmdId[0] && lastId == cmdId) || (cmdTs && cmdTs <= lastTs)) {
        publish_control_ack("forget_wifi", true, "ignored_duplicate");
        return;
      }

      // Persist the newest cmd markers BEFORE doing anything destructive
      if (pGuard.begin("myApp", false)) {
        pGuard.putULong64("forget_ts", cmdTs ? cmdTs : (uint64_t)time(nullptr)*1000ULL);
        if (cmdId[0]) pGuard.putString("forget_id", cmdId);
        pGuard.end();
      }

      Preferences p;
      if (p.begin("myApp", false)) {
        p.putUInt("wifiAuthType", (uint32_t)WIFI_CONN_PSK);
        p.putString("wifiSSID", "");
        p.putString("wifiPassword", "");
        p.putString("eapUsername", "");
        p.putString("eapPassword", "");
        p.putString("eapIdentity", "");
        p.putString("eapAnon", "");
        p.end();
      }

      // 2) Recharger les globals RAM (cohérence)
      prefs.begin("myApp", true);
      wifiSSID     = prefs.getString("wifiSSID", "");
      wifiPassword = prefs.getString("wifiPassword", "");
      wifiAuthType = (WifiAuthType)prefs.getUInt("wifiAuthType",(uint32_t)WIFI_CONN_PSK);
      eapIdentity  = prefs.getString("eapIdentity", "");
      eapUsername  = prefs.getString("eapUsername", "");
      eapPassword  = prefs.getString("eapPassword", "");
      eapAnon      = prefs.getString("eapAnon", "");
      prefs.end();

      // 3) ACK immédiat (non retained)
      publish_control_ack("forget_wifi", true);

      // 4) Publier un état RETAINED "erasing" pour l'app (feedback UX)
      publish_status_retained("erasing", "forget_wifi");

      // 5) Clear le retained /control pour éviter ré-application post-reboot
      String ctrlTopic = mqtt_topic_ctrl();
      s_mqtt.publish(ctrlTopic.c_str(), "", true);
      s_mqtt.loop();
      delay(80);

      // 6) Vide au mieux la file de publish utilisateurs
      mqtt_flush(200);

      // 7) Couper proprement MQTT + Wi-Fi
      s_mqtt.disconnect();
      WiFi.disconnect(true, true);

      // 8) LED → pairing (visuel immédiat) puis redémarrage "propre"
      updateLedState(LED_PAIRING);
      delay(120);
      ESP.restart();
      return;
    }

    if (strcmp(action, "factory_reset")==0) {
      Serial.println("[RESET] Factory reset demandé via MQTT");
      DynamicJsonDocument ack(128);
      ack["ack"]="factory_reset";
      ack["ok"]=true;
      String out; serializeJson(ack, out);
      s_mqtt.publish(mqtt_topic_status().c_str(), out.c_str(), false);

      g_factoryResetPending = true;
      xTaskCreatePinnedToCore([](void*){
        vTaskDelay(50/portTICK_PERIOD_MS);
        // le reset réel est géré dans main (doFactoryResetOnMainLoop)
        vTaskDelete(NULL);
      }, "FACTORY_RESET", 4096, NULL, 1, NULL, 0);
      return;
    }
    return;
  }

  if (t == mqtt_topic_ota()) {
    // si tu veux des commandes OTA brutes ici, à traiter au besoin
    return;
  }
}

static const char* makePemZ(const uint8_t* start, const uint8_t* end) {
  static std::string buf;
  buf.assign((const char*)start, (const char*)end);
  buf.push_back('\0');
  return buf.c_str();
}
// ======= Connexion broker =======
static bool mqtt_do_connect() {
  if (!wifiConnected || !timeIsSane()) return false;
  if (g_netBusyForOta || otaIsInProgress()) return false;
  s_tls.setCACert(CA_BUNDLE_PEM);

  s_tls.setTimeout(10000);

  s_mqtt.setServer(MQTT_HOST, MQTT_PORT);   // SNI OK car on passe un hostname
  s_mqtt.setBufferSize(2048);
  s_mqtt.setKeepAlive(30);
  s_mqtt.setSocketTimeout(10);
  s_mqtt.setCallback(onMqttMessage);


  // LastWill retained offline
  DynamicJsonDocument lw(96);
  lw["state"]="offline";
  lw["ts"]=(uint64_t)(time(nullptr)*1000ULL);
  static String lwtPayload; lwtPayload.clear(); serializeJson(lw, lwtPayload);

  String envSuffix = String(MQTT_PREFIX).startsWith("prod/") ? "prod" : "dev";
  String clientId = "breezly-sensor-" + envSuffix + "-" + sensorId;

  Serial.printf("[MQTT] trying clientId=%s host=%s port=%d\n",
                clientId.c_str(), MQTT_HOST, MQTT_PORT);
  bool ok = s_mqtt.connect(
    clientId.c_str(),
    MQTT_USER, MQTT_PASS,
    mqtt_topic_status().c_str(),
    0, true,
    lwtPayload.c_str()
  );
  if (!ok) {
    int state = s_mqtt.state();
    Serial.printf("[MQTT] connect FAIL state=%d time=%ld\n", state, time(nullptr));
    char ctx[80];
    snprintf(ctx, sizeof(ctx), "{\"reason\":%d}", state);
    mqtt_telemetry_emit("MQTT_CONNECT_FAIL", ctx);
    return false;
  }
  s_mqttBackoff.reset();  // session établie → reset backoff
  mqtt_telemetry_emit("MQTT_CONNECT_OK", "{}");

  // Subscriptions
  s_mqtt.subscribe(mqtt_topic_ota().c_str(), 0);
  s_mqtt.subscribe(mqtt_topic_ctrl().c_str(), 0);

  // Boot message
  DynamicJsonDocument j(256);
  j["boot"]=true;
  j["sensorId"]=sensorId;
  j["userId"]=userId;
  j["firmwareVersion"]=CURRENT_FIRMWARE_VERSION;
  String s; serializeJson(j, s);
  s_mqtt.publish((String(MQTT_PREFIX) + "capteurs/boot").c_str(), s.c_str(), false);

  // Telemetry: boot_count + reboot loop detection (NVS window 10 min)
  {
    Preferences bootPrefs;
    bootPrefs.begin("boot", false);
    uint32_t bootCount = bootPrefs.getUInt("count", 0) + 1;
    bootPrefs.putUInt("count", bootCount);
    String tsList = bootPrefs.getString("ts", "");
    const uint32_t nowSec = (uint32_t)time(nullptr);
    const uint32_t windowSec = 10 * 60;
    int bootsInWindow = 0;
    if (tsList.length()) {
      int idx = 0;
      while (idx < (int)tsList.length()) {
        int end = tsList.indexOf(',', idx);
        if (end < 0) end = tsList.length();
        String part = tsList.substring(idx, end);
        uint32_t t = (uint32_t)part.toInt();
        if (nowSec - t <= windowSec) bootsInWindow++;
        idx = end + 1;
      }
    }
    bootsInWindow++;
    String newTs = tsList.length() ? (String(nowSec) + "," + tsList) : String(nowSec);
    int count = 0;
    int cut = -1;
    for (int i = 0; i < (int)newTs.length(); i++) {
      if (newTs[i] == ',') count++;
      if (count >= 5) { cut = i; break; }
    }
    if (cut > 0) {
      int comma = newTs.lastIndexOf(',', cut - 1);
      newTs = comma >= 0 ? newTs.substring(0, comma) : newTs.substring(0, cut);
    }
    bootPrefs.putString("ts", newTs);
    bootPrefs.end();
    if (bootsInWindow >= 3) {
      const char* rr = "unknown";
      switch (esp_reset_reason()) {
        case ESP_RST_BROWNOUT: rr = "brownout"; break;
        case ESP_RST_PANIC:    rr = "panic";    break;
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:      rr = "wdt";      break;
        default:               rr = "other";   break;
      }
      char loopCtx[120];
      snprintf(loopCtx, sizeof(loopCtx), "{\"boots10min\":%d,\"lastResetReason\":\"%s\"}", bootsInWindow, rr);
      mqtt_telemetry_emit("FW_REBOOT_LOOP", loopCtx);
    }
  }
  // Telemetry: FW_BOOT with reset_reason, boot_count, brownout_flag, build_id, partition
  {
    const char* rr = "unknown";
    switch (esp_reset_reason()) {
      case ESP_RST_POWERON:  rr = "power_on";     break;
      case ESP_RST_EXT:      rr = "external";    break;
      case ESP_RST_SW:       rr = "software";    break;
      case ESP_RST_PANIC:    rr = "panic";       break;
      case ESP_RST_INT_WDT:  rr = "int_wdt";     break;
      case ESP_RST_TASK_WDT: rr = "task_wdt";    break;
      case ESP_RST_WDT:      rr = "wdt";         break;
      case ESP_RST_DEEPSLEEP:rr = "deep_sleep";  break;
      case ESP_RST_BROWNOUT: rr = "brownout";    break;
      case ESP_RST_SDIO:     rr = "sdio";        break;
      default:               rr = "unknown";     break;
    }
    bool brownout = (esp_reset_reason() == ESP_RST_BROWNOUT);
    Preferences bootPrefs;
    bootPrefs.begin("boot", true);
    uint32_t bootCount = bootPrefs.getUInt("count", 1);
    bootPrefs.end();
    const esp_partition_t* run = esp_ota_get_running_partition();
    const char* partLabel = run ? run->label : "unknown";
    DynamicJsonDocument ctx(320);
    ctx["reset_reason"] = rr;
    ctx["boot_count"] = bootCount;
    ctx["brownout_flag"] = brownout;
    ctx["fw_version"] = CURRENT_FIRMWARE_VERSION;
    ctx["build_id"] = BUILD_ID;
    ctx["partition"] = partLabel;
    String ctxStr;
    serializeJson(ctx, ctxStr);
    mqtt_telemetry_emit("FW_BOOT", ctxStr.c_str());
  }
  // Telemetry: OTA_SUCCESS from previous boot (fromVersion, toVersion)
  {
    Preferences p;
    if (p.begin("ota", true)) {
      String successVer = p.getString("success_ver", "");
      String fromVer = p.getString("from_ver", "");
      p.end();
      if (successVer.length()) {
        DynamicJsonDocument ctx(200);
        ctx["toVersion"] = successVer;
        ctx["fromVersion"] = fromVer.length() ? fromVer : CURRENT_FIRMWARE_VERSION;
        String ctxStr;
        serializeJson(ctx, ctxStr);
        mqtt_telemetry_emit("OTA_SUCCESS", ctxStr.c_str());
        p.begin("ota", false);
        p.remove("success_ver");
        p.remove("from_ver");
        p.end();
      }
    }
  }

  publish_status_retained("online");
  {
    DynamicJsonDocument h(96);
    h["hello"] = true;
    h["ts"]    = (uint64_t)(time(nullptr) * 1000ULL);
    String hs; serializeJson(h, hs);
    String helloTopic = mqtt_topic_device_base() + "/hello";
    s_mqtt.publish(helloTopic.c_str(), hs.c_str(), true);

    s_hello_ok = true;

    breezly_on_mqtt_hello_ok();
    maybe_fire_connected_final();
    ledOnConnectedOk();

  }

  // 2) S’abonner au topic status pour capter “registered”
  s_mqtt.subscribe(mqtt_topic_status().c_str(), 0);

  return true;
}

static const uint32_t TELEMETRY_HEARTBEAT_MS = 5 * 60 * 1000;  // 5 min
static uint32_t s_lastHeartbeatMs = 0;
static uint32_t s_minFreeHeap = 0xFFFFFFFFU;

// ======= Tâche propriétaire =======
static void mqttTask(void*) {
  s_queue = xQueueCreate(24, sizeof(PubMsg));
  s_ev    = xEventGroupCreate();
  s_ready = true;
    Serial.println("[MQTT] mqtttask " + String(millis()));
  for (;;) {
    if (g_netBusyForOta || otaIsInProgress()) {
      if (s_connected) { s_mqtt.disconnect(); s_connected = false; }
      vTaskDelay(100 / portTICK_PERIOD_MS);
      continue;
    }
    // (Re)connexion avec backoff exponentiel (Wi‑Fi down → pas de retry actif, juste delay)
    if (!s_connected) {
      bool shouldTry = false;
      EventBits_t bits = xEventGroupClearBits(s_ev, EV_REQ_CONNECT_BIT);
      if (bits & EV_REQ_CONNECT_BIT) shouldTry = true;
      if (!shouldTry && wifiConnected && s_mqttBackoff.shouldAttempt(millis())) {
        shouldTry = true;
      }
      if (shouldTry) {
        s_lastConnAttemptMs = millis();
        ensureTlsClockReady(20000);
        time_t now = time(nullptr);
        Serial.printf("[MQTT] pre-connect, unix=%ld sane=%d\n", (long)now, (int)timeIsSaneHard());
        if (!timeIsSaneHard()) { vTaskDelay(250/portTICK_PERIOD_MS); continue; }
        if (mqtt_do_connect()) {
          s_connected = true;
        } else {
          s_mqttBackoff.onFailure(millis(), 0);
          Serial.printf("[MQTT] backoff next in %lu ms\n", (unsigned long)s_mqttBackoff.lastDelayMs());
        }
      }
      vTaskDelay(50 / portTICK_PERIOD_MS);
      continue;
    }


    // Loop broker
    if (!s_mqtt.loop()) {
      s_connected = false;
      vTaskDelay(100 / portTICK_PERIOD_MS);
      continue;
    }

    PubMsg m;
    int drained = 0;
    while (drained < 8 && xQueueReceive(s_queue, &m, 0) == pdTRUE) {
      if (!s_mqtt.publish(m.topic, m.payload, m.retain)) {
        char pubFailCtx[180];
        snprintf(pubFailCtx, sizeof(pubFailCtx), "{\"topic\":\"%.80s\",\"code\":%d}", m.topic ? m.topic : "", (int)s_mqtt.state());
        mqtt_telemetry_emit("MQTT_PUBLISH_FAIL", pubFailCtx);
      }
      free(m.topic);
      free(m.payload);
      drained++;
    }

    // Telemetry heartbeat every 5 min
    uint32_t now = millis();
    if (s_lastHeartbeatMs == 0) s_lastHeartbeatMs = now;
    if ((now - s_lastHeartbeatMs) >= TELEMETRY_HEARTBEAT_MS) {
      s_lastHeartbeatMs = now;
      uint32_t freeHeap = (uint32_t)esp_get_free_heap_size();
      if (freeHeap < s_minFreeHeap) s_minFreeHeap = freeHeap;
      char ctx[260];
      snprintf(ctx, sizeof(ctx),
               "{\"uptime_ms\":%lu,\"wifi_rssi\":%d,\"free_heap\":%lu,\"min_free_heap\":%lu,\"mqtt_connected\":%s,\"night_mode\":%s}",
               (unsigned long)now, WiFi.RSSI(), (unsigned long)freeHeap, (unsigned long)s_minFreeHeap,
               s_connected ? "true" : "false", ledGetNightMode() ? "true" : "false");
      mqtt_telemetry_emit("FW_HEARTBEAT", ctx);
    }

    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void mqtt_bus_start_task() {
  xTaskCreatePinnedToCore(mqttTask, "MQTT_BUS", 16384, nullptr, 1, nullptr, 1);
}
