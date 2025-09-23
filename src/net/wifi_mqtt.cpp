#include "wifi_mqtt.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include "../core/globals.h"
#include "../ota/ota.h"
#include "ble/provisioning.h"

// Broker: tes valeurs
static const char* MQTT_HOST = "607207c4394d44b8bad11a33e8ed591d.s1.eu.hivemq.cloud";
static const int   MQTT_PORT = 8883;
static const char* MQTT_USER = "admin";
static const char* MQTT_PASS = "26052004Sg";

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
  if (wifiSSID.isEmpty() || wifiPassword.isEmpty()){
    Serial.println("[WiFi] SSID/PWD manquants");
    provisioningSetStatus("{\"status\":\"missing_fields\"}");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  Serial.printf("[WiFi] Connexion à '%s'...\n", wifiSSID.c_str());
  for (int i=0;i<20 && WiFi.status()!=WL_CONNECTED;i++){ delay(500); Serial.print("."); }

  if (WiFi.status()==WL_CONNECTED){
    Serial.printf("\n[WiFi] OK IP=%s\n", WiFi.localIP().toString().c_str());
    wifiConnected = true;

    // ✅ C’est ICI qu’on notifie l’app que c’est bon
    provisioningNotifyConnected();

    return true;
  } else {
    Serial.println("\n[WiFi] ÉCHEC");
    wifiConnected = false;

    provisioningSetStatus("{\"status\":\"error\"}");

    if (!bleInited && !bleInitStarted) {
      bleInitStarted = true;
      Serial.println("[BLE] Init différée -> démarrage maintenant (après échec Wi-Fi).");
      setupBLE(true);
    } else if (bleInited) {
      restartBLEAdvertising();
    }
    return false;
  }
}


bool connectToMQTT(){
  if (!wifiConnected) return false;
  tlsClient.setInsecure();
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setBufferSize(1024);
  mqttClient.setCallback(mqttCallback);
  Serial.print("[MQTT] Connexion ... ");
  if (mqttClient.connect("ESP32Client", MQTT_USER, MQTT_PASS)){
    Serial.println("OK"); return true;
  } else {
    Serial.printf("FAIL state=%d\n", mqttClient.state()); return false;
  }
}

void mqttSubscribeOtaTopic(){
  String otaTopic = "breezly/devices/" + sensorId + "/ota";
  if (mqttClient.subscribe(otaTopic.c_str())) Serial.printf("[MQTT] Subscribed: %s\n", otaTopic.c_str());
  else                                        Serial.printf("[MQTT] Subscribe FAIL: %s\n", otaTopic.c_str());
}

void mqttLoopOnce(){ mqttClient.loop(); }
