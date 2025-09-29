#include "wifi_mqtt.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include "../core/globals.h"
#include "../ota/ota.h"
#include "ble/provisioning.h"
#include "esp_task_wdt.h"
#include <time.h>
#include <string>
#include "nvs_flash.h"
#include "nvs.h"
#include "../led/led_status.h"
#include <ArduinoJson.h>
extern "C" {
  #include "esp_wifi.h"
  UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t xTask);
}
// Broker: tes valeurs
static volatile bool s_mqttConnTaskRunning = false;
static uint32_t      s_mqttAttemptCount    = 0;
// en haut de wifi_mqtt.cpp
static constexpr uint32_t MQTT_CONN_STACK = 16384; // mots FreeRTOS (~64 KB)

static const char* MQTT_HOST = "607207c4394d44b8bad11a33e8ed591d.s1.eu.hivemq.cloud";
static const int   MQTT_PORT = 8883;
static const char* MQTT_USER = "admin";
static const char* MQTT_PASS = "26052004Sg";
static unsigned long s_lastMqttAttemptMs = 0;
static uint32_t      s_mqttBackoffMs     = 1000;   // 1s initial
static const uint32_t MQTT_BACKOFF_MAX   = 60000;  // 60s max

// wifi_mqtt.cpp (en haut)
enum class MqttTlsPolicy : uint8_t { Insecure, CaOnly, MutualTLS };
static MqttTlsPolicy s_mqttTls = MqttTlsPolicy::CaOnly; // défaut sûr

// Certs embarqués (via board_build.embed_txtfiles)
extern const uint8_t _binary_src_certs_hivemq_ca_pem_start[];
extern const uint8_t _binary_src_certs_hivemq_ca_pem_end[];
// Optionnel (mTLS entreprise)
extern const uint8_t _binary_src_certs_client_cert_pem_start[];
extern const uint8_t _binary_src_certs_client_cert_pem_end[];
extern const uint8_t _binary_src_certs_client_key_pem_start[];
extern const uint8_t _binary_src_certs_client_key_pem_end[];
static String statusTopic();
static void publishStatusRetained(const char* state, const char* reason=nullptr);
// --- helpers pour convertir le binaire linké en chaîne PEM terminée par NUL ---
static const char* makePemZ(const uint8_t* start, const uint8_t* end) {
  size_t len = (size_t)(end - start);
  // on garde les buffers statiques pour que les pointeurs restent valides
  static std::string buf;
  buf.assign((const char*)start, (const char*)end); // copie exacte
  buf.push_back('\0'); // NUL terminal
  return buf.c_str();
}

static void configureMqttTLS() {
  switch (s_mqttTls) {
    case MqttTlsPolicy::Insecure:
      tlsClient.setInsecure();
      break;

    case MqttTlsPolicy::CaOnly: {
      const char* ca_pem = makePemZ(_binary_src_certs_hivemq_ca_pem_start,
                                    _binary_src_certs_hivemq_ca_pem_end);
      tlsClient.setCACert(ca_pem);
      Serial.println("[MQTT/TLS] CA only");
      break;
    }

    case MqttTlsPolicy::MutualTLS: {
      const char* ca_pem   = makePemZ(_binary_src_certs_hivemq_ca_pem_start,
                                      _binary_src_certs_hivemq_ca_pem_end);
      const char* crt_pem  = makePemZ(_binary_src_certs_client_cert_pem_start,
                                      _binary_src_certs_client_cert_pem_end);
      const char* key_pem  = makePemZ(_binary_src_certs_client_key_pem_start,
                                      _binary_src_certs_client_key_pem_end);

      tlsClient.setCACert(ca_pem);
      tlsClient.setCertificate(crt_pem);
      tlsClient.setPrivateKey(key_pem);
      Serial.println("[MQTT/TLS] mutual TLS");
      break;
    }
  }

  tlsClient.setTimeout(5000);
  mqttClient.setSocketTimeout(5);
}


static void erasePrefsAndWifi() {
  // Efface namespace app
  Preferences p;
  if (p.begin("myApp", false)) {  // RW
    p.clear();                    // supprime toutes les clés du namespace
    p.end();
  }

  // Efface config Wi-Fi (SSID, pass, EAP…) stockée par l'ESP32 dans NVS
  esp_wifi_restore(); // remet les paramètres Wi-Fi à l’état d’usine

  // (optionnel & radical) : tout NVS de la puce -> à éviter si tu stockes autre chose globalement
  // nvs_flash_erase(); 
  // nvs_flash_init();
}

static void gracefulStopBeforeReset() {
  updateLedState(LED_UPDATING);    // ou LED_OFF si tu préfères
  Serial.println("[RESET] Arrêt propre avant reset...");
  Serial.println("o7");
  // coupe tes tâches capteurs / PMS proprement
  // coupe BLE adv pour éviter des races
}

static void factoryResetAndReboot() {
  gracefulStopBeforeReset();

  // Annonce état au cloud
  StaticJsonDocument<128> s; s["state"]="erasing";
  String sMsg; serializeJson(s,sMsg);
  mqttClient.publish(("breezly/devices/"+sensorId+"/status").c_str(), sMsg.c_str());

  delay(150); // laisse partir le publish

  // Déconnexion réseau propre
  if (mqttClient.connected()) mqttClient.disconnect();
  WiFi.disconnect(true); // coupe la station (true = wifioff)

  erasePrefsAndWifi();

  delay(200);
  ESP.restart();
}
static bool applyWifiPrefsFromJson(const JsonDocument& j, String& errMsg) {
  Preferences p;
  if (!p.begin("myApp", false)) { errMsg = "prefs_begin_failed"; return false; }

  // Auth: "psk" ou "eap"
  const char* auth = j["authType"] | "psk";
  WifiAuthType newAuth = (strcmp(auth,"eap")==0) ? WIFI_CONN_EAP_PEAP_MSCHAPV2 : WIFI_CONN_PSK;

  const char* ssid = j["ssid"] | "";
  const char* pwd  = j["password"] | "";

  // Champs EAP optionnels
  const char* eapUser = j["eap"]["username"] | "";
  const char* eapPass = j["eap"]["password"] | "";
  const char* eapId   = j["eap"]["identity"] | "";
  const char* eapAnon = j["eap"]["anon"] | "";

  // Validations minimales
  if (newAuth == WIFI_CONN_PSK) {
    if (strlen(ssid) == 0 || strlen(pwd) == 0) { errMsg="missing_ssid_or_pwd"; return false; }
  } else {
    if (!ssid || !eapUser || !eapPass) { errMsg="missing_eap_fields"; p.end(); return false; }
  }
  Serial.printf("[CTRL] set_wifi -> auth=%s, ssid_len=%u, pwd_len=%u\n",
              auth, (unsigned)strlen(ssid), (unsigned)strlen(pwd));
  // Ecrit les valeurs (on n'imprime pas les secrets dans les logs)
  p.putUInt("wifiAuthType", (uint32_t)newAuth);
  p.putString("wifiSSID",     ssid ? ssid : "");
  p.putString("wifiPassword", pwd  ? pwd  : "");

  if (newAuth == WIFI_CONN_EAP_PEAP_MSCHAPV2) {
    p.putString("eapUsername", eapUser ? eapUser : "");
    p.putString("eapPassword", eapPass ? eapPass : "");
    p.putString("eapIdentity", eapId   ? eapId   : "");
    p.putString("eapAnon",     eapAnon ? eapAnon : "anon@domain");
  } else {
    // nettoie les anciens champs EAP s'il y en avait
    p.putString("eapUsername", "");
    p.putString("eapPassword", "");
    p.putString("eapIdentity", "");
    p.putString("eapAnon",     "");
  }

  p.end();
  return true;
}
// Publie un ACK sur le topic status
static void publishControlAck(const char* type, bool ok, const char* reason=nullptr){
  StaticJsonDocument<160> j;
  j["ack"]   = type;   // ex: "set_wifi"
  j["ok"]    = ok;
  j["ts"]    = (uint64_t)(time(nullptr) * 1000ULL);
  if (reason) j["reason"] = reason;
  String s; serializeJson(j, s);
  mqttClient.publish(statusTopic().c_str(), s.c_str()); // pas besoin de retained
}

static void handleSetWifi(const JsonDocument& j){
  String err;
  bool ok = applyWifiPrefsFromJson(j, err);
  publishControlAck("set_wifi", ok, ok ? nullptr : err.c_str());
  if (!ok) return;

  // (re)charge les globals
  prefs.begin("myApp", true);
  wifiSSID     = prefs.getString("wifiSSID", "");
  wifiPassword = prefs.getString("wifiPassword", "");
  wifiAuthType = (WifiAuthType)prefs.getUInt("wifiAuthType",(uint32_t)WIFI_CONN_PSK);
  eapIdentity  = prefs.getString("eapIdentity", "");
  eapUsername  = prefs.getString("eapUsername", "");
  eapPassword  = prefs.getString("eapPassword", "");
  eapAnon      = prefs.getString("eapAnon", "");
  prefs.end();

  // ⚠️ efface le retained AVANT de couper MQTT/WiFi
  String ctrlTopic = "breezly/devices/" + sensorId + "/control";
  bool cleared = mqttClient.publish(ctrlTopic.c_str(), "", true);
  Serial.printf("[CTRL] clear retained on %s -> %d\n", ctrlTopic.c_str(), (int)cleared);
  mqttClient.loop();
  delay(50);

  if (mqttClient.connected()) mqttClient.disconnect();
  WiFi.disconnect(true, true);

  wifiConnected     = false;
  needToConnectWiFi = true;
  updateLedState(LED_BOOT);
}


static void mqttCallback(char* topic, byte* payload, unsigned int length){
  Serial.printf("[MQTT] RX topic=%s len=%u\n", topic, length);
  if (length == 0) { 
    // retained cleared → rien à faire
    return; 
  }
  // reconstruire le message
  String msg; msg.reserve(length + 1);
  for (unsigned i=0;i<length;i++) msg += (char)payload[i];
  Serial.printf("[MQTT] RX payload=%s\n", msg.c_str());

  // parse JSON
  StaticJsonDocument<512> j;
  DeserializationError err = deserializeJson(j, msg);
  if (err) {
    Serial.printf("[MQTT] JSON parse error: %s\n", err.c_str());
    return;
  }

  // récupérer l'action proprement
  const char* action = j["action"].as<const char*>();
  if (!action) {
    Serial.println("[MQTT] Pas de clé 'action' (ou non-string)");
    return;
  }
  Serial.printf("[MQTT] action=%s\n", action);
  if (strcmp(action, "set_wifi") == 0) {
    // Ex: { "action":"set_wifi", "authType":"psk", "ssid":"...", "password":"..." }
    // ou EAP: { "action":"set_wifi", "authType":"eap", "ssid":"eduroam",
    //           "eap": { "username":"u", "password":"p", "identity":"u", "anon":"anon@realm" } }
    handleSetWifi(j);
    return;
  }
  // OTA
  if (strcmp(action, "update") == 0){
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

  // Factory reset
  if (strcmp(action, "factory_reset") == 0){
    Serial.println("[RESET] Factory reset demandé via MQTT");

    // ACK immédiat
    StaticJsonDocument<128> ack;
    ack["ack"] = "factory_reset";
    ack["ok"]  = true;
    String out; serializeJson(ack, out);
    mqttClient.publish(("breezly/devices/"+sensorId+"/status").c_str(), out.c_str());
    g_factoryResetPending = true;
    // reset asynchrone
    xTaskCreatePinnedToCore([](void*){
      vTaskDelay(50/portTICK_PERIOD_MS);
      factoryResetAndReboot();
      vTaskDelete(NULL);
    }, "FACTORY_RESET", 8192, NULL, 1, NULL, 0);
    return;
  }

  Serial.println("[MQTT] Action inconnue -> ignorée");
}

static bool bleInitStarted = false;

bool connectToWiFi(){
  if (wifiAuthType == WIFI_CONN_EAP_PEAP_MSCHAPV2) {
    bool ok = connectToWiFiEnterprise();
    if (ok) provisioningNotifyConnected();
    else {
      provisioningSetStatus("{\"status\":\"error\"}");
      if (bleInited) restartBLEAdvertising();
    }
    return ok;
  }

  // ===== PSK sécurisé WDT =====
  if (wifiSSID.isEmpty() || wifiPassword.isEmpty()){
    Serial.println("[WiFi] SSID/PWD manquants");
    provisioningSetStatus("{\"status\":\"missing_fields\"}");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);                 // reset complet
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  Serial.printf("[WiFi] Connexion à '%s'...\n", wifiSSID.c_str());

  const uint32_t timeoutMs = 15000;           // 15 s
  const uint32_t deadline  = millis() + timeoutMs;
  while (WiFi.status() != WL_CONNECTED && (int32_t)(deadline - millis()) > 0) {
    esp_task_wdt_reset();                     // ★ feed WDT
    vTaskDelay(100 / portTICK_PERIOD_MS);     // ★ laisse tourner FreeRTOS
    if (((millis()/500) % 2) == 0) Serial.print(".");
  }
  Serial.println();

  if (WiFi.status()==WL_CONNECTED){
    Serial.printf("[WiFi] OK IP=%s RSSI=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    wifiConnected = true;
    provisioningNotifyConnected();
    startSNTPAfterConnected();                // comme EAP
    return true;
  } else {
    Serial.println("[WiFi] ÉCHEC (timeout)");
    wifiConnected = false;
    provisioningSetStatus("{\"status\":\"error\"}");
    if (bleInited) restartBLEAdvertising();   // UX: permet de corriger les creds
    return false;
  }
}


static bool timeIsSane(){ return time(nullptr) > 1700000000; }

bool connectToMQTT(){
  if (!wifiConnected) return false;
  if (!timeIsSane())  startSNTPAfterConnected();
  // Timeouts courts pour éviter de bloquer loopTask
  configureMqttTLS(); 
  tlsClient.setTimeout(5000);       // *** important ***
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setBufferSize(1024);
  mqttClient.setSocketTimeout(5);   // *** important ***
  mqttClient.setCallback(mqttCallback);

  ++s_mqttAttemptCount;
  unsigned long t0 = millis();
  Serial.printf("[MQTT] Tentative #%lu -> %s:%d ... ",
                (unsigned long)s_mqttAttemptCount, MQTT_HOST, MQTT_PORT);
  StaticJsonDocument<96> lw; 
  lw["state"] = "offline"; 
  lw["ts"]    = (uint64_t)(time(nullptr) * 1000ULL);
  static String lwtPayload; lwtPayload.clear(); serializeJson(lw, lwtPayload);

  // ⚠️ setWill garde un pointeur -> garder lwtPayload vivant jusqu'au connect()
  bool ok = mqttClient.connect(
    "ESP32Client",
    MQTT_USER, MQTT_PASS,
    statusTopic().c_str(),   // willTopic
    0,                       // willQos
    true,                    // willRetain
    lwtPayload.c_str()       // willMessage
  );
  unsigned long dt = millis() - t0;
  if (ok) {
    Serial.printf("OK (%lums)\n", dt);
    publishStatusRetained("online");
    return true;
  } else {
    Serial.printf("FAIL state=%d (%lums)\n", mqttClient.state(), dt);
    return false;
  }
}
static void mqttConnectTask(void*){
  bool ok = connectToMQTT();
  if (ok) {
    mqttSubscribeOtaTopic();
    s_mqttBackoffMs = 1000;
  }
  s_mqttConnTaskRunning = false;
  UBaseType_t hw = uxTaskGetStackHighWaterMark(NULL);
  Serial.printf("[MQTT] HighWaterMark=%lu words (~%lu bytes)\n",
                (unsigned long)hw, (unsigned long)hw*4);
  vTaskDelete(NULL);
}

void scheduleMqttConnect(){
  if (g_factoryResetPending) return;
  if (s_mqttConnTaskRunning) return;
  s_mqttConnTaskRunning = true;
  xTaskCreatePinnedToCore(mqttConnectTask, "MQTT_CONN", MQTT_CONN_STACK, nullptr, 1, nullptr, 0);
}


void mqttSubscribeOtaTopic(){
  String otaTopic   = "breezly/devices/" + sensorId + "/ota";
  String ctrlTopic  = "breezly/devices/" + sensorId + "/control";

  bool ok1 = mqttClient.subscribe(otaTopic.c_str());
  Serial.printf("[MQTT] Subscribed OTA: %s (%s)\n", otaTopic.c_str(), ok1?"ok":"fail");

  bool ok2 = mqttClient.subscribe(ctrlTopic.c_str());
  Serial.printf("[MQTT] Subscribed CTRL: %s (%s)\n", ctrlTopic.c_str(), ok2?"ok":"fail");
}

// --- Status helpers ---
static String statusTopic() { return "breezly/devices/" + sensorId + "/status"; }
static void publishStatusRetained(const char* state, const char* reason) {
  StaticJsonDocument<128> j;
  j["state"] = state;
  j["ts"]    = (uint64_t)(time(nullptr) * 1000ULL);
  if (reason) j["reason"] = reason;
  String s; serializeJson(j, s);
  mqttClient.publish(statusTopic().c_str(), s.c_str(), true); // retained
}

void mqttLoopOnce(){
  if (g_factoryResetPending) return;

  // ⛔ ne touche pas mqttClient pendant qu'on connecte
  if (s_mqttConnTaskRunning) return;

  if (!wifiConnected){
    if (mqttClient.connected()){
      mqttClient.disconnect();
      Serial.println("[MQTT] Déconnecté (WiFi KO)");
    }
    return;
  }

  if (mqttClient.connected()){
    mqttClient.loop();
    return;
  }

  unsigned long now = millis();
  if ((long)(now - s_lastMqttAttemptMs) >= (long)s_mqttBackoffMs){
    s_lastMqttAttemptMs = now;
    scheduleMqttConnect();
    s_mqttBackoffMs = s_mqttBackoffMs < MQTT_BACKOFF_MAX
      ? min<uint32_t>(MQTT_BACKOFF_MAX, s_mqttBackoffMs << 1)
      : MQTT_BACKOFF_MAX;
    Serial.printf("[MQTT] Retry planifié dans tâche (backoff=%lums)\n", (unsigned long)s_mqttBackoffMs);
  }
}

