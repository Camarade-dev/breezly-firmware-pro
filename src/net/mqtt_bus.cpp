#include "mqtt_bus.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>
#include <Preferences.h>
#include "../core/globals.h"     // wifiConnected, sensorId, userId, prefs, CURRENT_FIRMWARE_VERSION
#include "../led/led_status.h"
#include "../ota/ota.h"
#include "../net/sntp_utils.h"
#include <string>
#include "../ca_bundle.h"

#include "../app_config.h"  
// ======= PARAMS BROKER (reprends les tiens) =======
static const char* MQTT_HOST = "607207c4394d44b8bad11a33e8ed591d.s1.eu.hivemq.cloud";
static const int   MQTT_PORT = 8883;
static const char* MQTT_USER = "admin";
static const char* MQTT_PASS = "26052004Sg";

// ======= Ressources de la tâche MQTT =======
static WiFiClientSecure s_tls;
static PubSubClient     s_mqtt(s_tls);
static QueueHandle_t    s_queue = nullptr;
static EventGroupHandle_t s_ev   = nullptr;

static volatile bool s_connected = false;
static volatile bool s_ready     = false;

static const int EV_REQ_CONNECT_BIT = (1 << 0);
static uint32_t s_lastConnAttemptMs = 0;
static const uint32_t RECONNECT_BACKOFF_MS = 8000; // 8s

// ======= Topics helpers =======
String mqtt_topic_device_base() { return "breezly/devices/" + sensorId; }
String mqtt_topic_ota()    { return mqtt_topic_device_base() + "/ota"; }
String mqtt_topic_ctrl()   { return mqtt_topic_device_base() + "/control"; }
String mqtt_topic_status() { return mqtt_topic_device_base() + "/status"; }

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
  PubMsg m{
    strdup(t.c_str()),
    strdup(p.c_str()),
    qos,
    retain
  };
  if (!m.topic || !m.payload) { if (m.topic) free(m.topic); if (m.payload) free(m.payload); return false; }
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

  if (t == mqtt_topic_ctrl()) {
    StaticJsonDocument<512> j;
    auto err = deserializeJson(j, msg);
    if (err) { Serial.printf("[MQTT] JSON parse error: %s\n", err.c_str()); return; }
    const char* action = j["action"] | "";
    if (!action[0]) return;

    if (strcmp(action, "set_wifi")==0) { handleSetWifi(j); return; }

    if (strcmp(action, "update")==0) {
      Serial.println("[OTA] Trigger via MQTT");
      otaInProgress = true;
      xTaskCreatePinnedToCore([](void*){
        vTaskDelay(100/portTICK_PERIOD_MS);
        checkAndPerformCloudOTA();
        otaInProgress = false;
        vTaskDelete(NULL);
      }, "OTA_TASK", 8192, NULL, 1, NULL, 0);
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

  s_tls.setCACert(CA_BUNDLE_PEM);
  s_tls.setTimeout(10000);

  s_mqtt.setServer(MQTT_HOST, MQTT_PORT);
  s_mqtt.setBufferSize(2048);
  s_mqtt.setKeepAlive(30);
  s_mqtt.setSocketTimeout(10);
  s_mqtt.setCallback(onMqttMessage);

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

  String clientId = "breezly_" + sensorId;
bool ok = s_mqtt.connect(
    clientId.c_str(),
    MQTT_USER, MQTT_PASS,
    mqtt_topic_status().c_str(),
    0, true,
    lwtPayload.c_str()
  );
  if (!ok) {
    Serial.printf("[MQTT] connect FAIL state=%d time=%ld\n", s_mqtt.state(), time(nullptr));
    return false;
  }

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
  s_mqtt.publish("capteurs/boot", s.c_str(), false);

  publish_status_retained("online");
  return true;
}

// ======= Tâche propriétaire =======
static void mqttTask(void*) {
  s_queue = xQueueCreate(24, sizeof(PubMsg));
  s_ev    = xEventGroupCreate();
  s_ready = true;

  for (;;) {
    // (Re)connexion
    if (!s_connected) {
      bool shouldTry = false;
      EventBits_t bits = xEventGroupClearBits(s_ev, EV_REQ_CONNECT_BIT);
      if (bits & EV_REQ_CONNECT_BIT) shouldTry = true;
      if (!shouldTry && wifiConnected && (millis() - s_lastConnAttemptMs) >= RECONNECT_BACKOFF_MS) {
        shouldTry = true;
      }
      if (shouldTry) {
        s_lastConnAttemptMs = millis();

        // <<<< AJOUT
        ensureTlsClockReady(20000);
        time_t now = time(nullptr);
        Serial.printf("[MQTT] pre-connect, unix=%ld sane=%d\n", (long)now, (int)timeIsSaneHard());
        if (!timeIsSaneHard()) { vTaskDelay(250/portTICK_PERIOD_MS); continue; }
        // >>>>

        if (mqtt_do_connect()) s_connected = true;
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
    s_mqtt.publish(m.topic, m.payload, m.retain);  // QoS/retain comme avant
    free(m.topic);
    free(m.payload);
    drained++;
    }

    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void mqtt_bus_start_task() {
  xTaskCreatePinnedToCore(mqttTask, "MQTT_BUS", 16384, nullptr, 1, nullptr, 1);
}
