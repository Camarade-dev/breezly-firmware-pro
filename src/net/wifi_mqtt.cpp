#include "wifi_mqtt.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include "../core/globals.h"
#include "../ota/ota.h"
#include "ble/provisioning.h"
#include "esp_task_wdt.h"
#include <time.h>
#include <string>

extern "C" UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t xTask);
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




static void mqttCallback(char* topic, byte* payload, unsigned int length){
  String msg; msg.reserve(length);
  for (unsigned i=0;i<length;i++) msg += (char)payload[i];
  if (msg.indexOf("\"action\"")!=-1 && msg.indexOf("update")!=-1){
    Serial.println("[OTA] Trigger via MQTT");
    otaInProgress = true;
    xTaskCreatePinnedToCore([](void*){
      vTaskDelay(100/portTICK_PERIOD_MS);
      checkAndPerformCloudOTA();
      otaInProgress = false;
      vTaskDelete(0);
    }, "OTA_TASK", 8192, 0, 1, 0, 0);
  }
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

  // ==== Chemin PSK (ton code d'origine) ====
  if (wifiSSID.isEmpty() || wifiPassword.isEmpty()){
    Serial.println("[WiFi] SSID/PWD manquants");
    provisioningSetStatus("{\"status\":\"missing_fields\"}");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  Serial.printf("[WiFi] Connexion à '%s'...\n", wifiSSID.c_str());
  for (int i=0;i<20 && WiFi.status()!=WL_CONNECTED; i++){
    delay(500); Serial.print(".");
  }

  if (WiFi.status()==WL_CONNECTED){
    Serial.printf("\n[WiFi] OK IP=%s\n", WiFi.localIP().toString().c_str());
    wifiConnected = true;
    provisioningNotifyConnected();
    startSNTPAfterConnected();   // SNTP aussi pour PSK
    return true;
  } else {
    Serial.println("\n[WiFi] ÉCHEC");
    wifiConnected = false;
    provisioningSetStatus("{\"status\":\"error\"}");
    if (bleInited) restartBLEAdvertising();
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

  bool ok = mqttClient.connect("ESP32Client", MQTT_USER, MQTT_PASS);

  unsigned long dt = millis() - t0;
  if (ok) {
    Serial.printf("OK (%lums)\n", dt);
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
  }
  s_mqttConnTaskRunning = false;
  UBaseType_t hw = uxTaskGetStackHighWaterMark(NULL);
  Serial.printf("[MQTT] HighWaterMark=%lu words (~%lu bytes)\n",
                (unsigned long)hw, (unsigned long)hw*4);
  vTaskDelete(NULL);
}

void scheduleMqttConnect(){
  if (s_mqttConnTaskRunning) return;
  s_mqttConnTaskRunning = true;
  xTaskCreatePinnedToCore(mqttConnectTask, "MQTT_CONN", MQTT_CONN_STACK, nullptr, 1, nullptr, 0);
}


void mqttSubscribeOtaTopic(){
  String otaTopic = "breezly/devices/" + sensorId + "/ota";
  if (mqttClient.subscribe(otaTopic.c_str())) Serial.printf("[MQTT] Subscribed: %s\n", otaTopic.c_str());
  else                                        Serial.printf("[MQTT] Subscribe FAIL: %s\n", otaTopic.c_str());
}

void mqttLoopOnce(){
  // Si Wi-Fi KO → on s’assure que MQTT est down proprement
  if (!wifiConnected){
    if (mqttClient.connected()){
      mqttClient.disconnect();
      Serial.println("[MQTT] Déconnecté (WiFi KO)");
    }
    return;
  }

  // Wi-Fi OK
  if (mqttClient.connected()){
    mqttClient.loop();
    return;
  }

  // Wi-Fi OK mais MQTT KO → retry avec backoff, en tâche séparée
  unsigned long now = millis();
  if ((long)(now - s_lastMqttAttemptMs) >= (long)s_mqttBackoffMs){
    s_lastMqttAttemptMs = now;
    scheduleMqttConnect();  // <-- au lieu d'appeler connectToMQTT() ici
    // backoff exponentiel (il sera réduit si on connecte)
    s_mqttBackoffMs = s_mqttBackoffMs < MQTT_BACKOFF_MAX
      ? min<uint32_t>(MQTT_BACKOFF_MAX, s_mqttBackoffMs << 1)
      : MQTT_BACKOFF_MAX;
    Serial.printf("[MQTT] Retry planifié dans tâche (backoff=%lums)\n", (unsigned long)s_mqttBackoffMs);
  }
}
