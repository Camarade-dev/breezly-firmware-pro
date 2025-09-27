#include <Arduino.h>
#include "app_config.h"

#include "core/globals.h"
#include "utils/crc_utils.h"

#include "led/led_status.h"
#include "ble/provisioning.h"
#include "net/wifi_mqtt.h"
#include "ota/ota.h"
#include "sensors/sensors.h"
#include <WiFi.h>

#include "esp_task_wdt.h"
#include <ArduinoJson.h>
static volatile bool s_otaBootTaskScheduled = false;
static volatile bool s_otaTickTaskScheduled = false;
static const unsigned long WIFI_RETRY_BACKOFF_MS = 15000; // 15s
static const uint8_t WIFI_FAILS_BEFORE_PROV = 1;          // dès le 1er échec, on ouvre BLE

// --- Trampo pour init BLE sur core 0 ---
static bool s_bleStartAdv = false;


void setup(){
  delay(4000);
  Serial.begin(115200);
  Serial.printf("Flash chip size: %u MB\n", ESP.getFlashChipSize()/(1024*1024));
  otaOnBootValidate(); 
  esp_task_wdt_deinit();
  

  ledInit(LED_PIN, LED_COUNT);
  updateLedState(LED_BOOT);
  

  WiFi.disconnect(true);
  prefs.begin("myApp", true);
  wifiSSID     = prefs.getString("wifiSSID", "");
  wifiPassword = prefs.getString("wifiPassword", "");
  sensorId     = prefs.getString("sensorId", "");
  userId       = prefs.getString("userId", "");
  wifiAuthType = (WifiAuthType)prefs.getUInt("wifiAuthType", (uint32_t)WIFI_CONN_PSK);
  // EAP
  eapIdentity  = prefs.getString("eapIdentity", "");
  eapUsername  = prefs.getString("eapUsername", "");
  eapPassword  = prefs.getString("eapPassword", "");
  eapAnon      = prefs.getString("eapAnon", "ano@rezoleo.fr");
  prefs.end();

  Serial.printf("authType=%u (0=PSK,1=EAP)\n", (unsigned)wifiAuthType);
  if (wifiAuthType == WIFI_CONN_EAP_PEAP_MSCHAPV2) {
    Serial.printf("EAP SSID: %s\n", wifiSSID.c_str());
    Serial.printf("EAP user: %s\n", eapUsername.c_str());
    Serial.printf("EAP anon: %s\n", eapAnon.c_str());
  } else {
    Serial.printf("SSID: %s\n", wifiSSID.c_str());
  }


  bool valid  = preferencesAreValid();
  bool manque = (wifiSSID.isEmpty() || wifiPassword.isEmpty());
  bool needProv = (!valid || manque);


  Serial.println("----- PREFS -----");
  Serial.printf("SSID: %s\n", wifiSSID.c_str());
  Serial.printf("PWD : %s\n", wifiPassword.c_str());
  Serial.printf("SID : %s\n", sensorId.c_str());
  Serial.printf("UID : %s\n", userId.c_str());
  Serial.printf("valid=%d manque=%d needProv=%d\n", (int)valid,(int)manque,(int)needProv);
  Serial.println("-----------------");

  setupBLE(needProv);  // ← démarre BLE tout de suite si provisioning requis

  // (optionnel) attendre un peu:
  uint32_t t0 = millis();
  while(!bleInited && millis()-t0 < 2000) { esp_task_wdt_reset(); delay(10); }

  ledTaskStart();
  // 2) Tenter la connexion Wi-Fi immédiate si on a des identifiants
  //    (en cas d’échec, connectToWiFi() s’occupe de relancer l’advertising BLE)
  if (!manque) {
    Serial.println("Tentative de connexion avec les identifiants sauvegardés...");
    connectToWiFi();
  }

  // 3) Si provisioning nécessaire : LED pairing + attente jusqu’à connexion Wi-Fi
  if (needProv) {
    updateLedState(LED_PAIRING);
    Serial.println("En attente provisioning BLE...");
    while (!wifiConnected) {
      delay(500);
      esp_task_wdt_reset();
    }
    Serial.println("Wi-Fi connecté, provisioning terminé.");
  }
  esp_task_wdt_config_t twdt_cfg = {};
  twdt_cfg.timeout_ms     = 30 * 1000;
  twdt_cfg.idle_core_mask = ((1U << portNUM_PROCESSORS) - 1U);
  twdt_cfg.trigger_panic  = true;
  ESP_ERROR_CHECK( esp_task_wdt_init(&twdt_cfg) );

  esp_task_wdt_add(NULL);
  if (wifiConnected && !mqttClient.connected()) {
    Serial.println("[MQTT] Tentative initiale post-WiFi");
    scheduleMqttConnect();   // <-- asynchrone, ne bloque pas loopTask
  }
  sensorsInit();
  gPmsMutex = xSemaphoreCreateMutex();
  pmsTaskStart(16, 17);

  if (mqttClient.connected()){
    StaticJsonDocument<256> j;
    j["boot"]=true; j["sensorId"]=sensorId; j["firmwareVersion"]=CURRENT_FIRMWARE_VERSION;
    String s; serializeJson(j,s);
    mqttClient.publish("capteurs/boot", s.c_str());
    Serial.println(s);
  }

  Serial.println("[BOOT] Setup terminé");
  lastWifiAttemptMs = millis();
}

void loop(){
  if (otaIsInProgress()) {
    // Pas de publish capteurs, pas de mqttClient.loop()
    esp_task_wdt_reset();
    delay(10);
  } 
  else{
    // <- ton code “normal” (publish capteurs, mqttClient.loop(), etc.)
  

  // ★ Check OTA au premier boot Wi-Fi, 1 seule fois
  if (wifiConnected && !otaIsInProgress() && lastOtaCheck==0 && !s_otaBootTaskScheduled){
    Serial.println("[OTA] Check au boot Wi-Fi");
    lastOtaCheck = millis();
    s_otaBootTaskScheduled = true;
    xTaskCreatePinnedToCore([](void*){
      // Cette task n’empêche plus loopTask de tourner
      esp_task_wdt_add(NULL);
      checkAndPerformCloudOTA();
      esp_task_wdt_delete(NULL);
      vTaskDelete(NULL);
    }, "OTA_BOOT", 8192, nullptr, 1, nullptr, 0);

  }

  // ★ Check OTA périodique
  if (!otaInProgress && wifiConnected){
    unsigned long now = millis();
    if ((long)(now - lastOtaCheck) >= (long)OTA_CHECK_INTERVAL_MS && !s_otaTickTaskScheduled){
      lastOtaCheck = now;
      s_otaTickTaskScheduled = true;
      xTaskCreatePinnedToCore([](void*){
          ledSuspend();
          checkAndPerformCloudOTA();   // reboot si update OK
          ledResume();                 // relance seulement si pas de reboot
        s_otaTickTaskScheduled = false;
        vTaskDelete(NULL);
      }, "OTA_TICK", 16384, NULL, 1, NULL, 0);
    }
  }

  // Provisioning → nouveaux identifiants reçus
  if (!wifiConnected && needToConnectWiFi){
    needToConnectWiFi = false;   // évite plusieurs déclenchements
    Serial.println("[WiFi] tentative suite à nouveaux identifiants (BLE)");
    connectToWiFi(); // échec => restartBLEAdvertising() à l'intérieur comme avant
    if (wifiConnected) {
      updateLedState(LED_BOOT);
      scheduleMqttConnect();   // <-- asynchrone
    } else {
      updateLedState(LED_PAIRING);
    }
  }

  // Publish capteurs
  if (mqttClient.connected()){
    unsigned long now = millis();
    if ((long)(now - lastPublish) >= 5000){
      lastPublish = now;

      float t,h;
      if (safeSensorRead(t,h)){
        int aqi,tvoc,eco2; sensorsReadEns160(aqi,tvoc,eco2,t,h);

        if (h>=40 && h<=60) updateLedState(LED_GOOD);
        else if ((h>=20&&h<40)||(h>60&&h<=70)) updateLedState(LED_MODERATE);
        else updateLedState(LED_BAD);

        PmsData p; bool have=false;
        if (gPmsMutex && xSemaphoreTake(gPmsMutex, 5/portTICK_PERIOD_MS)==pdTRUE){
          p = gPms; xSemaphoreGive(gPmsMutex);
          have = p.valid && (millis()-p.lastMs < 5000);
        }

        StaticJsonDocument<512> j;
        j["temperature"]=t; j["humidity"]=h; j["AQI"]=aqi; j["TVOC"]=tvoc; j["eCO2"]=eco2;
        if (have){
          JsonObject atm = j["pms"]["atm"].to<JsonObject>();   atm["pm1"]=p.pm1_atm;   atm["pm25"]=p.pm25_atm; atm["pm10"]=p.pm10_atm;
          JsonObject cf1 = j["pms"]["cf1"].to<JsonObject>();   cf1["pm1"]=p.pm1_cf1;   cf1["pm25"]=p.pm25_cf1; cf1["pm10"]=p.pm10_cf1;
          JsonObject cnt = j["pms"]["counts"].to<JsonObject>();cnt["gt03"]=p.gt03; cnt["gt05"]=p.gt05; cnt["gt10"]=p.gt10; cnt["gt25"]=p.gt25; cnt["gt50"]=p.gt50; cnt["gt100"]=p.gt100;
        }
        j["sensorId"]=sensorId; j["userId"]=userId;

        String s; serializeJson(j,s);
        mqttClient.publish("capteurs/qualite_air", s.c_str());
        Serial.println(s);
      } else {
        updateLedState(LED_BAD);
      }
    }
  }
  }


  mqttLoopOnce();
  esp_task_wdt_reset();
  delay(5);
}