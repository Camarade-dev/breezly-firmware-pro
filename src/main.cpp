#include <Arduino.h>
#include "app_config.h"

#include "core/globals.h"
#include "utils/crc_utils.h"

#include "led/led_status.h"
#include "ble/provisioning.h"
#include "net/mqtt_bus.h"
#include "ota/ota.h"
#include "sensors/sensors.h"
#include <WiFi.h>
#include "esp_wifi.h"
#include "net/wifi_connect.h"
#include "esp_task_wdt.h"
#include <ArduinoJson.h>

static bool s_twdtReady = false;
void doFactoryResetOnMainLoop(){
  updateLedState(LED_UPDATING);
  Serial.println("[RESET] Arrêt propre avant reset...");
  // stoppe proprement tes tâches capteurs, BLE adv, etc.

  // annonce "erasing" pendant que MQTT est encore up
  StaticJsonDocument<64> s; s["state"]="erasing";
  String sMsg; serializeJson(s, sMsg);
  mqtt_enqueue(mqtt_topic_status(), sMsg, 0, false);
  mqtt_flush(300); // on vide rapidement la queue
  delay(150); // laisse partir le publish

  WiFi.disconnect(true);             // coupe la STA

  // NVS / prefs
  Preferences p;
  if (p.begin("myApp", false)) { p.clear(); p.end(); }
  esp_wifi_restore();                // reset Wi-Fi factory

  delay(200);
  ESP.restart();
}

static void twdtInitOnce() {
  if (s_twdtReady) return;

  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms     = 120 * 1000;                          // your 2 min
  cfg.idle_core_mask = ((1U << portNUM_PROCESSORS) - 1U);   // feed IDLE0/1
  cfg.trigger_panic  = true;

  esp_err_t e = esp_task_wdt_init(&cfg);
  if (e == ESP_ERR_INVALID_STATE) {
    // already init’d with another timeout → tear down and re-init
    esp_task_wdt_deinit();                  // <-- important
    ESP_ERROR_CHECK( esp_task_wdt_init(&cfg) );
  } else {
    ESP_ERROR_CHECK(e);
  }

  // (re-)add current task (loopTask)
  e = esp_task_wdt_add(NULL);
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(e);

  s_twdtReady = true;
}


static inline void twdtResetSafe(){
  if (s_twdtReady) esp_task_wdt_reset();
}

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
  twdtInitOnce();
    

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

  // --- Vérifs d’identifiants selon le mode sélectionné ---
  const bool manquePSK = (wifiSSID.isEmpty() || wifiPassword.isEmpty());
  const bool manqueEAP = (wifiSSID.isEmpty() || eapUsername.isEmpty() || eapPassword.isEmpty());
  const bool missingCreds = (wifiAuthType == WIFI_CONN_PSK) ? manquePSK : manqueEAP;

  // "valid" = prefs cohérentes pour l'AUTH courante, pas forcément connectées
  const bool validAuth =
      (wifiAuthType == WIFI_CONN_PSK) ? !manquePSK : !manqueEAP;

  // needProv = on a besoin de provisioning BLE uniquement si les champs REQUIS
  // pour l'AUTH courante ne sont pas tous présents
  const bool needProv = !validAuth;

  Serial.println("----- PREFS -----");
  Serial.printf("authType=%u (0=PSK,1=EAP)\n", (unsigned)wifiAuthType);
  Serial.printf("SSID: %s\n", wifiSSID.c_str());
  if (wifiAuthType == WIFI_CONN_PSK) {
    Serial.printf("PWD : %s\n", wifiPassword.c_str());
  } else {
    Serial.printf("EAP user: %s\n", eapUsername.c_str());
    Serial.printf("EAP mdp : %s\n", eapPassword.c_str());
    Serial.printf("EAP anon: %s\n", eapAnon.c_str());
  }
  Serial.printf("validAuth=%d manquePSK=%d manqueEAP=%d needProv=%d\n",
                (int)validAuth, (int)manquePSK, (int)manqueEAP, (int)needProv);
  Serial.println("-----------------");



  Serial.println("----- PREFS -----");
  Serial.printf("SSID: %s\n", wifiSSID.c_str());
  Serial.printf("PWD : %s\n", wifiPassword.c_str());
  Serial.printf("SID : %s\n", sensorId.c_str());
  Serial.printf("UID : %s\n", userId.c_str());
  Serial.println("-----------------");

  setupBLE(needProv);  // ← démarre BLE tout de suite si provisioning requis

  // (optionnel) attendre un peu:
  uint32_t t0 = millis();
  while(!bleInited && millis()-t0 < 2000) { twdtResetSafe();
 delay(10); }

  ledTaskStart();
  sensorsInit();
  gPmsMutex = xSemaphoreCreateMutex();
  pmsTaskStart(16, 17);
  // 2) Tenter la connexion Wi-Fi immédiate si on a des identifiants
  //    (en cas d’échec, connectToWiFi() s’occupe de relancer l’advertising BLE)
  if (!missingCreds) {
    Serial.println("Tentative de connexion avec les identifiants sauvegardés...");
    connectToWiFi();
  }

  // 3) Si provisioning nécessaire : LED pairing + attente jusqu’à connexion Wi-Fi
  if (needProv) {
    updateLedState(LED_PAIRING);
    Serial.println("En attente provisioning BLE (non-bloquant)...");
    // NE PAS bloquer ici. loop() s'occupe d'appeler connectToWiFi()
  }
  twdtResetSafe(); delay(1);
  mqtt_bus_start_task();   // démarre le bus MQTT (tâche propriétaire)
  mqtt_request_connect();  // demande une première connexion

  Serial.println("[BOOT] Setup terminé");
  lastWifiAttemptMs = millis();
}

void loop(){
  if (g_factoryResetPending){
    g_factoryResetPending = false;

    doFactoryResetOnMainLoop();
    return; 
  }


  if (otaIsInProgress()) {
    twdtResetSafe();
    delay(10);
  } 
  else{
  // ★ Check OTA au premier boot Wi-Fi, 1 seule fois
  if (wifiConnected && !otaIsInProgress() && lastOtaCheck==0 && !s_otaBootTaskScheduled){
    Serial.println("[OTA] Check au boot Wi-Fi");
    lastOtaCheck = millis();
    s_otaBootTaskScheduled = true;
    xTaskCreatePinnedToCore([](void*){
      // ajoute la tâche au WDT si possible
      esp_err_t e = esp_task_wdt_add(NULL);
      if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) { ESP_ERROR_CHECK(e); }

      checkAndPerformCloudOTA();

      // la supprimer si elle était ajoutée
      e = esp_task_wdt_delete(NULL);
      if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) { ESP_ERROR_CHECK(e); }

      vTaskDelete(NULL);
    }, "OTA_BOOT", 8192, nullptr, 1, nullptr, 0);


  }

  // ★ Check OTA périodique
  if (!otaInProgress && wifiConnected && !g_factoryResetPending){
    unsigned long now = millis();
    if ((long)(now - lastOtaCheck) >= (long)OTA_CHECK_INTERVAL_MS && !s_otaTickTaskScheduled){
      lastOtaCheck = now;
      s_otaTickTaskScheduled = true;
      xTaskCreatePinnedToCore([](void*){
        // ajoute la tâche au WDT si possible
        esp_err_t e = esp_task_wdt_add(NULL);
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) { ESP_ERROR_CHECK(e); }

        checkAndPerformCloudOTA();

        // la supprimer si elle était ajoutée
        e = esp_task_wdt_delete(NULL);
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) { ESP_ERROR_CHECK(e); }

        vTaskDelete(NULL);
      }, "OTA_TICK", 8192, nullptr, 1, nullptr, 0);

    }
  }

  // Provisioning → nouveaux identifiants reçus
  if (!wifiConnected && needToConnectWiFi &&  !g_factoryResetPending){
    needToConnectWiFi = false;   // évite plusieurs déclenchements
    Serial.println("[WiFi] tentative suite à nouveaux identifiants (BLE)");
    twdtResetSafe();
    connectToWiFi(); // échec => restartBLEAdvertising() à l'intérieur comme avant
    twdtResetSafe();
    if (wifiConnected) {
      updateLedState(LED_BOOT);
      mqtt_request_connect();
    } else {
      updateLedState(LED_PAIRING);
    }
  }

  // Publish capteurs
  if (!g_factoryResetPending && mqtt_is_connected()){
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
        mqtt_enqueue("capteurs/qualite_air", s, 0, false);
        Serial.println(s);
      } else {
        updateLedState(LED_BAD);
      }
    }
  }
  }
  twdtResetSafe();
  delay(5);
}