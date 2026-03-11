#include <Arduino.h>
#include "app_config.h"

#include "core/globals.h"
#include "utils/crc_utils.h"
#include "power/sleep.h"
#include "led/led_status.h"
#include "ble/provisioning.h"
#include "net/mqtt_bus.h"
#include "power/power_config.h"
#include "ota/ota.h"
#include "sensors/sensors.h"
#include <WiFi.h>
#include "esp_wifi.h"
#include "net/wifi_connect.h"
#include "core/backoff.h"
#include "core/log.h"
#include "esp_task_wdt.h"
#include <esp_system.h>
#include <ArduinoJson.h>
#include "core/devkey_runtime.h"
#include "power/cpu_pm.h"
#include "sensors/calibration.h"
#ifndef FLASH_BUILD_SIG
#define FLASH_BUILD_SIG "nosig"
#endif

static bool s_twdtReady = false;
static bool s_mqttStarted = false;   // NEW : MQTT pas encore démarré
static bool s_firstPublishDone = false;
static bool s_otaValidFallbackDone = false;
// Indique que la fenêtre OTA de boot est terminée (succès, échec ou skip)
volatile bool g_otaBootWindowDone = false;

void doFactoryResetOnMainLoop(){
  updateLedState(LED_UPDATING);
  LOGI("RESET", "Arrêt propre avant reset...");
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
static unsigned long lastEns = 0;
static unsigned long lastPms = 0;

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
static bool s_needProv = false;  // besoin provisioning BLE (pas de creds valides) — set in setup
static const uint8_t WIFI_FAILS_BEFORE_PROV = 1;          // dès le 1er échec, on ouvre BLE

// --- Trampo pour init BLE sur core 0 ---
static bool s_bleStartAdv = false;
// Score continu de qualité d’air : 0 = très bon, 1 = très mauvais
// Score continu de qualité d’air : 0 = très bon, 1 = très mauvais

// Score continu de qualité d’air : 0 = très bon, 1 = très mauvais

static float clamp01(float v){
  if (v < 0.0f) return 0.0f;
  if (v > 1.0f) return 1.0f;
  return v;
}

// 0 = air top, 1 = air vraiment mauvais
// Couleur pilotée par le PIRE indicateur, mais avec seuils "soft"
// et courbe adoucie pour éviter de stresser l’utilisateur.
static float computeAirQualityScore(float tempC,
                                    float humidity,
                                    int   aqi,
                                    int   tvoc,
                                    int   eco2,
                                    float pm25_ux)
{
  // 1) Humidité : confort élargi 35–65%, 20–80 = extrêmes
  float humScore = 0.0f;
  if (humidity < 35.0f) {
    humScore = (35.0f - humidity) / (35.0f - 20.0f);   // 35 → 0, 20 → 1
  } else if (humidity > 65.0f) {
    humScore = (humidity - 65.0f) / (80.0f - 65.0f);   // 65 → 0, 80 → 1
  }
  humScore = clamp01(humScore);

  // 2) Température : 20–24°C idéal, 18–26 ok
  float tempScore = 0.0f;
  if (tempC < 20.0f) {
    tempScore = (20.0f - tempC) / 4.0f;        // 20 → 0, 16 → 1
  } else if (tempC > 24.0f) {
    tempScore = (tempC - 24.0f) / 4.0f;        // 24 → 0, 28 → 1
  }
  tempScore = clamp01(tempScore);

  // 3) AQI ENS160 (1–5) : mapping "soft"
  // 1 = excellent, 2 = bon, 3 = moyen, 4 = mauvais, 5 = très mauvais
  float aqiScore = 0.0f;
  switch (aqi) {
    case 1: aqiScore = 0.00f; break;  // excellent
    case 2: aqiScore = 0.10f; break;  // bon
    case 3: aqiScore = 0.25f; break;  // moyen léger
    case 4: aqiScore = 0.60f; break;  // mauvais
    case 5: aqiScore = 1.00f; break;  // très mauvais
    default:
      if (aqi <= 1)      aqiScore = 0.0f;
      else if (aqi >= 5) aqiScore = 1.0f;
      else               aqiScore = (float)(aqi - 1) / 4.0f;
      break;
  }
  aqiScore = clamp01(aqiScore);

  // 4) TVOC (ppb) – un peu plus tolérant
  //   ≤ 300 : neutre
  //   300–2000 : monte doucement
  //   ≥ 2000 : rouge
  float tvocScore = 0.0f;
  if (tvoc <= 300)          tvocScore = 0.0f;
  else if (tvoc >= 2000)    tvocScore = 1.0f;
  else                      tvocScore = (float)(tvoc - 300) / (2000.0f - 300.0f);
  tvocScore = clamp01(tvocScore);

  // 5) eCO2 (ppm)
  //   ≤ 900 : neutre
  //   900–2000 : monte
  //   ≥ 2000 : rouge
  float eco2Score = 0.0f;
  if (eco2 <= 900)          eco2Score = 0.0f;
  else if (eco2 >= 2000)    eco2Score = 1.0f;
  else                      eco2Score = (float)(eco2 - 900) / (2000.0f - 900.0f);
  eco2Score = clamp01(eco2Score);

  // 6) PM2.5 UX (µg/m³) (fusionné / lissé)
  //   0–10 : très bon
  //   10–20 : moyen
  //   20–35 : mauvais
  //   >35 : très mauvais
  float pm25Score = 0.0f;
  if (pm25_ux > 0.0f) {   // si pas dispo → 0 (ne pénalise pas)
    if (pm25_ux <= 10.0f)       pm25Score = 0.0f;
    else if (pm25_ux >= 35.0f)  pm25Score = 1.0f;
    else                        pm25Score = (pm25_ux - 10.0f) / (35.0f - 10.0f);
  }
  pm25Score = clamp01(pm25Score);

  // 7) PIRE indicateur
  float worst = humScore;
  if (tempScore > worst)  worst = tempScore;
  if (aqiScore  > worst)  worst = aqiScore;
  if (tvocScore > worst)  worst = tvocScore;
  if (eco2Score > worst)  worst = eco2Score;
  if (pm25Score > worst)  worst = pm25Score;

  // 8) Adoucir visuellement les états moyens :
  //    - 0 ou proche → reste très vert
  //    - valeurs moyennes tirent un peu vers le bas
  //    - les états vraiment mauvais restent bien rouges
  float softened = powf(worst, 1.4f);   // gamma doux > 1

  return clamp01(softened);
}

static void resetWifiOnEveryFlash() {
  Preferences boot;
  if (!boot.begin("boot", false)) return;

  String last = boot.getString("last_sig", "");
  String now  = String(FLASH_BUILD_SIG);

  if (last == now) {
    boot.end();
    return; // ✅ même firmware => pas de reset
  }

  LOGI("RESET", "New flash detected. last=%s now=%s", last.c_str(), now.c_str());

  // IMPORTANT: écrire la signature AVANT de faire des trucs destructifs,
  // sinon tu peux boucler si reboot/power loss pendant le reset
  boot.putString("last_sig", now);
  boot.end();

  // --- reset Wi-Fi ESP32 (ce que tu veux vraiment) ---
  WiFi.disconnect(true, true);
  delay(50);
  esp_wifi_restore();
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

  // (optionnel) si tu veux aussi vider TES prefs applicatives :
  // Preferences p;
  // if (p.begin("myApp", false)) { p.clear(); p.end(); }

  delay(100);
  ESP.restart(); // reboot propre (1 seule fois par flash)
}

void setup(){
  delay(500);
  Serial.begin(115200);
  delay(100);

  // Log reset reason et boot_count au boot (aligné télémétrie MQTT FW_BOOT)
  {
    const char* rr = "unknown";
    switch (esp_reset_reason()) {
      case ESP_RST_POWERON:  rr = "power_on";     break;
      case ESP_RST_EXT:      rr = "external";     break;
      case ESP_RST_SW:       rr = "software";     break;
      case ESP_RST_PANIC:    rr = "panic";        break;
      case ESP_RST_INT_WDT:  rr = "int_wdt";      break;
      case ESP_RST_TASK_WDT: rr = "task_wdt";    break;
      case ESP_RST_WDT:      rr = "wdt";          break;
      case ESP_RST_DEEPSLEEP:rr = "deep_sleep";   break;
      case ESP_RST_BROWNOUT: rr = "brownout";     break;
      case ESP_RST_SDIO:     rr = "sdio";         break;
      default:               rr = "unknown";      break;
    }
    bool brownout = (esp_reset_reason() == ESP_RST_BROWNOUT);
    Preferences bootPrefs;
    bootPrefs.begin("boot", true);
    uint32_t nvsCount = bootPrefs.getUInt("count", 0);
    bootPrefs.end();
    uint32_t bootCount = nvsCount + 1;  // valeur qui sera envoyée en FW_BOOT
    LOGI("BOOT", "reset_reason=%s boot_count=%lu brownout=%d", rr, (unsigned long)bootCount, (int)brownout);
  }

#if defined(BACKOFF_SIM_TEST)
  backoff_run_simulation();
#endif

#if FACTORY_RESET_ON_FLASH
  resetWifiOnEveryFlash();
#endif

  enableCpuPM();
  otaOnBootValidate(); 
  twdtInitOnce();
    

  ledInit(LED_PIN, LED_COUNT);
  updateLedState(LED_BOOT);
  ledOnBoot();

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
  eapAnon      = prefs.getString("eapAnon", "");
  eapInsecure  = prefs.getBool  ("eapInsecure", false);
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

  LOGD("BOOT", "PREFS authType=%u (0=PSK,1=EAP)", (unsigned)wifiAuthType);
  LOGD("BOOT", "PREFS SSID=%s validAuth=%d needProv=%d", wifiSSID.c_str(), (int)validAuth, (int)needProv);

  loadOrInitDevKey();
  LOGD("BOOT", "DEVKEY len=%d redacted", (int)g_deviceKeyB64.length());
  LOGD("BOOT", "PREFS SID=%s UID=%s", sensorId.c_str(), userId.c_str());
  s_needProv = needProv;
  setupBLE(needProv);  // ← démarre BLE tout de suite si provisioning requis

  // (optionnel) attendre un peu:
  uint32_t t0 = millis();
  while(!bleInited && millis()-t0 < 2000) { twdtResetSafe();
 delay(10); }

  // ⚠️ On NE démarre plus les capteurs / PMS ici.
  // On les démarrera après la fenêtre OTA de boot, une fois qu'on a
  // soit mis à jour, soit décidé de rester sur la version actuelle.
  ledTaskStart(); 
  // sensorsInit();
  // calInit();
  // calCompose(); 
  // gPmsMutex = xSemaphoreCreateMutex();
  // pmsTaskStart(16, 17);
  // pmsInitPins(15); // SET=15
  // pmsSleep();
  // 2) Tenter la connexion Wi-Fi immédiate si on a des identifiants
  //    (en cas d’échec, connectToWiFi() s’occupe de relancer l’advertising BLE)
  if (!missingCreds) {
    LOGI("BOOT", "Tentative de connexion avec les identifiants sauvegardés...");
    connectToWiFi();
  }

  // 3) Si provisioning nécessaire : LED pairing + attente jusqu’à connexion Wi-Fi
  if (needProv) {
    LOGI("BOOT", "En attente provisioning BLE (non-bloquant)...");
    // NE PAS bloquer ici. loop() s'occupe d'appeler connectToWiFi()
  }
  twdtResetSafe(); delay(1);
  // ⚠️ NE PLUS démarrer MQTT ici
  // mqtt_bus_start_task();
  // mqtt_request_connect();

  LOGI("BOOT", "Setup terminé");
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
    LOGI("OTA", "Check au boot Wi-Fi");
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
  if (!otaIsInProgress() && wifiConnected && !g_factoryResetPending){
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
    LOGI("WiFi", "tentative suite à nouveaux identifiants (BLE)");
    twdtResetSafe();
    connectToWiFi(); // échec => restartBLEAdvertising() à l'intérieur comme avant
    twdtResetSafe();
    if (wifiConnected) {
      mqtt_request_connect();
    } else {
      // En phase de provisioning BLE : si la tentative échoue,
      // on repasse en mode "besoin de provisioning" et on arrête
      // les retries automatiques avec backoff, et on coupe le handler
      // EAP pour éviter les logs STA_DISCONNECTED en boucle.
      s_needProv = true;
      wifiBackoffReset();
      wifi_enterprise_detach_disc_handler();
    }
  }

  // État LED : rouge clignotant si déconnecté, vert (qualité d’air) si connecté
  if (s_mqttStarted && !g_factoryResetPending && !otaIsInProgress()) {
    if (!wifiConnected || !mqtt_is_connected())
      updateLedState(LED_BAD);
    else
      updateLedState(LED_GOOD);  // reconnexion → vert tout de suite (score mis à jour au prochain publish)
  }

  // Fallback : marquer app VALID après 60 s si WiFi OK (au cas où MQTT ne connecte pas)
  if (!s_otaValidFallbackDone && millis() >= 60000 && wifiConnected && !otaIsInProgress()) {
    s_otaValidFallbackDone = true;
    otaMarkAppValidIfPending();
  }

  // Retry Wi‑Fi avec backoff exponentiel (si creds valides, pas en provisioning, pas OTA)
  if (!wifiConnected && !s_needProv && !g_factoryResetPending && !otaIsInProgress()
      && wifiBackoffShouldAttempt()) {
    lastWifiAttemptMs = millis();
    twdtResetSafe();
    connectToWiFi();
    twdtResetSafe();
    if (wifiConnected) {
      mqtt_request_connect();
    }
  }

  // Publish capteurs
  if (!g_factoryResetPending && mqtt_is_connected()){

    const unsigned long ENS_PERIOD = ENS_READ_PERIOD_MS_DAY;
    const unsigned long PMS_PERIOD = PMS_SAMPLE_PERIOD_MS_DAY;
    unsigned long nowMs = millis();

    bool doEns = false;
    bool doPms = false;

    // 🔹 1) Première mesure "instantanée" pour l'UX
    if (!s_firstPublishDone) {
      doEns = true;          // on envoie ENS tout de suite
      // doPms = true;       // → à activer si tu veux aussi forcer un PMS direct (je ne le recommande pas forcément)
      s_firstPublishDone = true;

      // On reset les fenêtres de période à partir de maintenant
      lastEns = nowMs;
      // lastPms = nowMs;    // seulement si tu forces aussi doPms ci-dessus
    }
    // 🔹 2) Ensuite, comportement normal périodique
    else {
      doEns = (long)(nowMs - lastEns) >= (long)ENS_PERIOD;
      doPms = (long)(nowMs - lastPms) >= (long)PMS_PERIOD;
    }

    float t,h, t_raw, h_raw;
    int aqi=0,tvoc=0,eco2=0;
    PmsData p = {}; bool havePms = false;
    float pressurePa = NAN, tempBmp = NAN;
    bool haveBmp = false;
    uint16_t co2NdirPpm = 0;
    float tempScd41 = NAN, humidityScd41 = NAN;
    bool haveScd41 = false;

    if (doEns){
      if (safeSensorRead(t, h, &t_raw, &h_raw)){
        sensorsReadEns160(aqi,tvoc,eco2,t,h);
        haveBmp = bmp581Read(pressurePa, tempBmp);
        haveScd41 = scd41Read(co2NdirPpm, tempScd41, humidityScd41);
        lastEns = nowMs;
      } else {
        doEns = false; // on annule si la lecture a foiré
      }
    }

    if (doPms){
      havePms = pmsSampleBlocking(PMS_WARMUP_MS, p);
      lastPms = nowMs;
    }

    if (doEns || doPms){
      float pm1f = NAN, pm25f = NAN, pm10f = NAN;
      bool sanityOk = true;
      char sanityFailBuf[32] = {};
      if (doEns){
        sanityOk = sensorSanityCheck(aqi, tvoc, eco2, sanityFailBuf, sizeof(sanityFailBuf));
      }

      if (havePms){
        // 1) FUSION + LISSSAGE → valeurs UX au dixième
        pmsPostProcess(p, pm1f, pm25f, pm10f);

        auto round1 = [](float v)->float {
          return floorf(v * 10.0f + 0.5f) / 10.0f; // arrondi au 0.1
        };

        // 2) Publis structurées (toujours envoyées ; sanity_ok pour le backend)
        StaticJsonDocument<512> j;
        if (doEns){
          j["temperature"]=t_raw; j["humidity"]=h_raw; j["AQI"]=aqi; j["TVOC"]=tvoc; j["eCO2"]=eco2;
          if (haveBmp && isfinite(pressurePa)) j["pressure_pa"] = (float)pressurePa;
          if (haveBmp && isfinite(tempBmp))     j["temperature_bmp"] = (float)tempBmp;
          if (haveScd41) { j["co2_ndir_ppm"] = co2NdirPpm; if (isfinite(tempScd41)) j["temperature_scd41"] = (float)tempScd41; if (isfinite(humidityScd41)) j["humidity_scd41"] = (float)humidityScd41; }
          j["sanity_ok"] = sanityOk;
          if (!sanityOk && sanityFailBuf[0]) j["sanity_fail"] = sanityFailBuf;
        }

        // --- ux : valeurs fusionnées/lissées (pour l’app & le backend)
        JsonObject ux = j["pms"]["ux"].to<JsonObject>();
        ux["pm1"]  = round1(pm1f);
        ux["pm25"] = round1(pm25f);
        ux["pm10"] = round1(pm10f);
        ux["source"] = "fused-v1";

        // --- atm : bruts du module
        JsonObject atm = j["pms"]["atm"].to<JsonObject>();
        atm["pm1"]  = p.pm1_atm;
        atm["pm25"] = p.pm25_atm;
        atm["pm10"] = p.pm10_atm;

        // --- cf1 : bruts CF=1
        JsonObject cf1 = j["pms"]["cf1"].to<JsonObject>();
        cf1["pm1"]  = p.pm1_cf1;
        cf1["pm25"] = p.pm25_cf1;
        cf1["pm10"] = p.pm10_cf1;

        // --- counts
        JsonObject cnt = j["pms"]["counts"].to<JsonObject>();
        cnt["gt03"]=p.gt03; cnt["gt05"]=p.gt05; cnt["gt10"]=p.gt10;
        cnt["gt25"]=p.gt25; cnt["gt50"]=p.gt50; cnt["gt100"]=p.gt100;

        j["sensorId"]=sensorId; j["userId"]=userId;
        String s; serializeJson(j,s);
        mqtt_enqueue("capteurs/qualite_air", s, 0, false);
        LOGD("MQTT", "qualite_air payload len=%u", (unsigned)s.length());
        mqtt_flush(200);
      } else {
        // cas sans PMS dispo : on envoie ENS uniquement (toujours envoyé ; sanity_ok pour le backend)
        StaticJsonDocument<512> j;
        if (doEns){
          j["temperature"]=t_raw; j["humidity"]=h_raw; j["AQI"]=aqi; j["TVOC"]=tvoc; j["eCO2"]=eco2;
          if (haveBmp && isfinite(pressurePa)) j["pressure_pa"] = (float)pressurePa;
          if (haveBmp && isfinite(tempBmp))     j["temperature_bmp"] = (float)tempBmp;
          if (haveScd41) { j["co2_ndir_ppm"] = co2NdirPpm; if (isfinite(tempScd41)) j["temperature_scd41"] = (float)tempScd41; if (isfinite(humidityScd41)) j["humidity_scd41"] = (float)humidityScd41; }
          j["sanity_ok"] = sanityOk;
          if (!sanityOk && sanityFailBuf[0]) j["sanity_fail"] = sanityFailBuf;
        }
        j["sensorId"]=sensorId; j["userId"]=userId;
        String s; serializeJson(j,s);
        mqtt_enqueue("capteurs/qualite_air", s, 0, false);
        LOGD("MQTT", "qualite_air payload len=%u", (unsigned)s.length());
        mqtt_flush(200);
      }

      // === Score qualité d’air continu → LED ===
      if (doEns){
        float pm25ForScore = isfinite(pm25f) ? pm25f : 0.0f;
        float airScore = computeAirQualityScore(t, h, aqi, tvoc, eco2, pm25ForScore);
        ledSetAirQualityScore(airScore);
      }

      ledNotifyPublish();

      // Fenêtre interactive courte après publish
      enterModemSleep(true);
      lightNapMs(INTERACTIVE_WINDOW_MS);
      enterModemSleep(false);
    }

  }
} 
        if (!s_mqttStarted
      && wifiConnected
      && g_otaBootWindowDone        // ✅ fenêtre OTA terminée
      && !otaIsInProgress()
      && !g_factoryResetPending) {

    LOGI("BOOT", "Start sensors + MQTT after OTA window");

    // ======= CAPTEURS / PMS =======
    if (!gPmsMutex) {
      gPmsMutex = xSemaphoreCreateMutex();
    }

    sensorsInit();
    calInit();
    calCompose();

    pmsTaskStart(16, 17);
    pmsInitPins(15);
    pmsSleep();

    // ======= MQTT =======
    mqtt_bus_start_task();
    mqtt_request_connect();
    s_mqttStarted = true;
  }


  twdtResetSafe();
  vTaskDelay(5/portTICK_PERIOD_MS);
  #if USE_DEEP_SLEEP
    if (!otaIsInProgress() && !g_factoryResetPending && mqtt_is_connected()){
      // Exemple : si nuit et rien à faire pendant 2 minutes -> deep sleep
        pmsSleep();
        deepSleepForMs(120000);
    }
  #endif
}