/**
 * Exemple complet pour :
 *  - Lecture d'identifiants Wi-Fi depuis Preferences
 *  - Connexion Wi-Fi si identifiants présents
 *  - Initialisation BLE pour provisioning
 *    - On démarre le BLE si pas d'identifiants, **ou** si la connexion Wi-Fi échoue
 *  - Notification de l'état via statusCharacteristic
 */
#include <Arduino.h>
#include "esp_crc.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include "esp_task_wdt.h"
#include "ScioSense_ENS160.h"
#include <HardwareSerial.h>
#include "freertos/semphr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <HTTPClient.h>
#include <Update.h>
#include "esp_bt.h"
#include "esp_heap_caps.h"
#include "mbedtls/sha256.h"
// ==== PROTOTYPES (obligatoires en .cpp quand les fonctions sont utilisées avant d'être définies)
void setupBLE(bool startAdvertising);
void restartBLEAdvertising();
void connectToWiFi();
bool connectToMQTT();
uint32_t computeChecksum(const String& ssid, const String& pwd, const String& sensor, const String& user);
bool isPreferencesValid();
bool safeSensorRead(float& tempC, float& humidity);
bool readPmsFrame(HardwareSerial &ser, struct PmsData &out);
void ledTask(void *parameter);
void reconnectionTask(void *parameter);
void pmsTask(void *parameter);
void configureAdvertising();
bool httpDownloadToUpdate(const String& binUrl);
void checkAndPerformCloudOTA();

enum LedMode {
  LED_BOOT,
  LED_PAIRING,
  LED_GOOD,
  LED_MODERATE,
  LED_BAD
};
void updateLedState(LedMode mode);
#define USE_PMS_COMMANDS 0
volatile LedMode currentLedMode = LED_BOOT;
volatile bool ledOverride = false;

void updateLedState(LedMode mode) {
  if (!ledOverride) {
    currentLedMode = mode;
  }
}
#define ENS160_ADDR  0x52
#define LED_PIN    13
#define LED_COUNT  1
#define CURRENT_FIRMWARE_VERSION "1.0.1"

Adafruit_NeoPixel led(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

int fadeValue = 0;
int fadeStep = 5;
bool fadeUp = true;
unsigned long lastFadeUpdate = 0;
const int fadeInterval = 30; // ms entre chaque update (ajuste la vitesse)


unsigned long lastPublish = 0;
const unsigned long publishInterval = 5000;
// ------------------------------------------------------------------
//              Fonction : Définir la couleur de la LED
// ------------------------------------------------------------------

// ===== Breezly OTA (cloud) =====
static const char* FW_MANIFEST_URL = "https://breezly-backend.onrender.com/firmware/esp32/wroom32e/prod/manifest.json";
// CURRENT_FIRMWARE_VERSION existe déjà chez toi
static const unsigned long OTA_CHECK_INTERVAL_MS = 1UL * 5UL * 60UL * 1000UL; // 6h (ajuste si tu veux)
// (Prod) Utilise un CA/pinning. Pour test, on peut passer en setInsecure().
/*
static const char* ROOT_CA = R"EOF(
-----BEGIN CERTIFICATE-----
... ton CA ...
-----END CERTIFICATE-----
)EOF";
*/
static volatile bool otaInProgress = false;
static unsigned long lastOtaCheck = 0;

// ------------------------------------------------------------------
//                          CONFIGURATION
// ------------------------------------------------------------------
const char* mqttServer = "607207c4394d44b8bad11a33e8ed591d.s1.eu.hivemq.cloud";
const int mqttPort = 8883;
const char* mqttUser = "admin";
const char* mqttPassword = "26052004Sg";

// ------------ Objets WiFi / MQTT ------------
WiFiClientSecure espClient;
PubSubClient client(espClient);

// ------------ Capteurs ------------
Adafruit_AHTX0 aht;
ScioSense_ENS160 ens160(ENS160_ADDR);

// ------------ Mémoire persistante ------------
Preferences prefs;

// ------------------------------------------------------------------
//      Déclarations & Variables pour la partie Provisioning BLE
// ------------------------------------------------------------------
static BLEUUID serviceUUID("60f8a11f-e56f-4c3c-9658-5578e2f8d754");
static BLEUUID credentialsCharacteristicUUID("d6f37ab1-4367-4001-b311-f219e161b736");
static BLEUUID statusCharacteristicUUID("11f47ab2-1111-4002-b316-f219e161b736");

static NimBLECharacteristic* credentialsCharacteristic = nullptr;
static NimBLECharacteristic* statusCharacteristic = nullptr;

// Variables globales pour Wi-Fi
String wifiSSID = "";
String wifiPassword = "";
bool needToConnectWiFi = false;
bool wifiConnected = false;

// Variables globales pour l'ID du capteur / user
String sensorId = "";
String userId = "";
// tout en haut
String gBleName; 

// Pointeur global vers le NimBLEServer (pour relancer le scan / pub si besoin)
NimBLEServer* pServer = nullptr;


// ------------------------------------------------------------------
//        CALLBACK : Réception des Credentials par BLE
// ------------------------------------------------------------------
static inline uint16_t be16(const uint8_t *b){ return (uint16_t)b[0]<<8 | b[1]; }
// --- PMS5003 (UART2) ---
#define PMS_RX 16   // ESP32 reçoit (depuis TX du PMS)
#define PMS_TX 17   // ESP32 envoie (vers RX du PMS)
HardwareSerial &PMS = Serial2;
struct PmsData;
bool readPmsFrame(HardwareSerial &ser, PmsData &out);
// Structure partagée pour les mesures PMS
struct PmsData {
  // Masses CF=1
  uint16_t pm1_cf1 = 0, pm25_cf1 = 0, pm10_cf1 = 0;
  // Masses ATM (ambiant)
  uint16_t pm1_atm = 0, pm25_atm = 0, pm10_atm = 0;
  // Comptages (par 0.1 L)
  uint16_t gt03 = 0, gt05 = 0, gt10 = 0, gt25 = 0, gt50 = 0, gt100 = 0;

  bool     valid   = false;
  uint32_t lastMs  = 0;
  uint32_t seq     = 0;
};
PmsData gPms;
SemaphoreHandle_t gPmsMutex;    // mutex pour protéger gPms

bool readPmsFrame(HardwareSerial &ser, PmsData &out) {
  while (ser.available() >= 32) {
    if ((uint8_t)ser.peek() != 0x42) { ser.read(); continue; }
    if (ser.available() < 2) return false;
    uint8_t b0 = ser.read(), b1 = ser.read();
    if (b0 != 0x42 || b1 != 0x4D) continue;

    uint8_t payload[30];
    if (ser.readBytes(payload, 30) != 30) return false;

    uint32_t sum = b0 + b1;
    for (int i=0;i<28;i++) sum += payload[i];
    uint16_t chk = be16(&payload[28]);
    if ((sum & 0xFFFF) != chk) return false;

    uint16_t frameLen  = be16(&payload[0]);
    if (frameLen != 28) return false;

    // Data1..12 : cf1 puis atm puis counts
    out.pm1_cf1  = be16(&payload[2]);
    out.pm25_cf1 = be16(&payload[4]);
    out.pm10_cf1 = be16(&payload[6]);

    out.pm1_atm  = be16(&payload[8]);
    out.pm25_atm = be16(&payload[10]);
    out.pm10_atm = be16(&payload[12]);

    out.gt03     = be16(&payload[14]);
    out.gt05     = be16(&payload[16]);
    out.gt10     = be16(&payload[18]);
    out.gt25     = be16(&payload[20]);
    out.gt50     = be16(&payload[22]);
    out.gt100    = be16(&payload[24]);

    out.valid    = true;
    out.lastMs   = millis();
    return true;
  }
  return false;
}

// Helper à ajouter
void configureAdvertising() {
  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
  if (!pAdvertising) return;

  NimBLEAdvertisementData advData;
  advData.setFlags(0x06);
  advData.setName(gBleName.c_str());
  advData.addServiceUUID(serviceUUID);
  pAdvertising->setAdvertisementData(advData);

  NimBLEAdvertisementData scanData;
  scanData.setName(gBleName.c_str());
  pAdvertising->setScanResponseData(scanData);
}


class CredentialsCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
    static String receivedChunks = "";
    Serial.println("Callback onWrite déclenché (Credentials)");

    // Récupérer le fragment
    std::string rawValue = pCharacteristic->getValue();
    String fragment = String(rawValue.c_str());

    Serial.print("Fragment reçu : ");
    Serial.println(fragment);

    // Ajouter le fragment au buffer
    receivedChunks += fragment;

    // On suppose fin du JSON quand on voit "}"
    if (receivedChunks.endsWith("}")) {
      Serial.println("Données complètes reçues : ");
      Serial.println(receivedChunks);

      // Parser
      StaticJsonDocument<192> doc;
      DeserializationError error = deserializeJson(doc, receivedChunks);
      if (!error) {
        Serial.println("JSON valide reçu.");

        // Extraire ssid/pwd
        const char* ssid = doc["ssid"];
        const char* password = doc["password"];
        // Extraire sensorId / userId (facultatifs)
        const char* sId = doc["sensorId"];
        const char* uId = doc["userId"];

        // Mise à jour Wi-Fi
        if (ssid && password) {
          wifiSSID = String(ssid);
          wifiPassword = String(password);
          Serial.println("Reçu SSID: " + wifiSSID);
          Serial.println("Reçu Password: " + wifiPassword);
          connectToWiFi();
          needToConnectWiFi = false;
          // Sauvegarder dans les préférences
          // Mise à jour sensorId / userId
          if (sId) {
            sensorId = String(sId);
          }
          if (uId) {
            userId = String(uId);
          }
          uint32_t checksum = computeChecksum(wifiSSID, wifiPassword, sensorId, userId);

          // Ouvre l'espace puis écris tout
          prefs.begin("myApp", false);
          prefs.putString("wifiSSID", wifiSSID);
          prefs.putString("wifiPassword", wifiPassword);
          prefs.putString("sensorId", sensorId);
          prefs.putString("userId", userId);
          prefs.putUInt("checksum", checksum);
          prefs.end();

        } else {
          Serial.println("JSON invalide ou ssid/password manquants");
        }
      } else {
        Serial.print("Erreur de parsing JSON : ");
        Serial.println(error.c_str());
      }
      // Réinitialiser le buffer
      receivedChunks = "";
    }
  }
};

// ------------------------------------------------------------------
//               SETUP BLE (Serveur + Caractéristiques)
// ------------------------------------------------------------------
void setupBLE(bool startAdvertising) {
  char bleName[25];
    Serial.printf("Heap free: %u, internal: %u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  prefs.begin("myApp", false);
  String storedBleName = prefs.getString("bleName", "");
  prefs.end();

  if (storedBleName == "") {
    uint64_t chipid = ESP.getEfuseMac();
    sprintf(bleName, "PROV_%012llX", chipid);
    prefs.begin("myApp", false);
    prefs.putString("bleName", bleName);
    prefs.end();
  } else {
    storedBleName.toCharArray(bleName, sizeof(bleName));
  }
Serial.println("dedans BLE def name1");
  gBleName = String(bleName);
Serial.println("dedans BLE def name1");
  NimBLEDevice::init(bleName);
  Serial.println("dedans BLE def name1");
  NimBLEDevice::setMTU(185); // max pour Android et iOS
  NimBLEDevice::setDeviceName(bleName); // ← aide certains scanners à voir le nom dans la scan response
Serial.println("dedans BLE def name2");
  pServer = NimBLEDevice::createServer();

  NimBLEService* pService = pServer->createService(serviceUUID);

  credentialsCharacteristic = pService->createCharacteristic(
    credentialsCharacteristicUUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  credentialsCharacteristic->setCallbacks(new CredentialsCallback());
  credentialsCharacteristic->createDescriptor("2901")->setValue("WiFi Credentials");
Serial.println("dedans BLE def name3");
  statusCharacteristic = pService->createCharacteristic(
    statusCharacteristicUUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  statusCharacteristic->createDescriptor("2901")->setValue("Device Status");
  statusCharacteristic->setValue("{\"status\":\"not_connected\"}");
Serial.println("dedans BLE def name4");
  pService->start();
  vTaskDelay(250 / portTICK_PERIOD_MS);
Serial.println("dedans BLE def name5");
  // ✅ Configure toujours l’advertising, même si on ne le démarre pas maintenant
  configureAdvertising();

  if (startAdvertising) {
    NimBLEDevice::getAdvertising()->start();
    Serial.printf("Advertising BLE démarré (%s).\n", bleName);
  } else {
    Serial.println("BLE prêt (no adv): identifiants Wi-Fi présents.");
  }
}

void restartBLEAdvertising() {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (!adv) return;
  configureAdvertising();   // ← s’assure que le nom + service sont bien présents
  adv->stop();
  vTaskDelay(50 / portTICK_PERIOD_MS);
  adv->start();
  Serial.println("Advertising relancé (après échec Wi-Fi).");
}


// ------------------------------------------------------------------
//         FONCTION : Connexion Wi-Fi
// ------------------------------------------------------------------
void connectToWiFi() {
  if (!wifiSSID.isEmpty() && !wifiPassword.isEmpty()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());

    Serial.println("Tentative de connexion au Wi-Fi...");
    int counter = 0;
    while (WiFi.status() != WL_CONNECTED && counter < 20) {
      delay(500);
      Serial.print(".");
      counter++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWi-Fi connecté !");
      Serial.print("IP : ");
      Serial.println(WiFi.localIP());
      wifiConnected = true;

      // Notifier la caractéristique
      if (statusCharacteristic) {
        String connectedStatus = "{\"status\":\"connected\"}";
        statusCharacteristic->setValue(connectedStatus);
        statusCharacteristic->notify();
      }
    } else {
  Serial.println("\nÉchec de connexion Wi-Fi");
  wifiConnected = false;

  if (statusCharacteristic) {
    statusCharacteristic->setValue("{\"status\":\"error\"}");
    statusCharacteristic->notify();
  }

  // ✅ Un seul endroit pour (re)lancer la pub, avec payload configuré
  restartBLEAdvertising();
}

  } else {
    Serial.println("Aucun identifiant Wi-Fi renseigné.");
  }
}
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Ex: topic = breezly/devices/<sensorId>/ota
  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  // Très simple: si on reçoit {"action":"update"} => check/MAJ
  if (msg.indexOf("\"action\"") != -1 && msg.indexOf("update") != -1) {
    Serial.println("[OTA] Trigger via MQTT");
    otaInProgress = true;
    // Lancer dans un petit task pour ne pas bloquer le callback
    xTaskCreatePinnedToCore([](void*){
      // petite latence pour laisser le callback finir
      vTaskDelay(100 / portTICK_PERIOD_MS);
      checkAndPerformCloudOTA();
      vTaskDelete(NULL);
    }, "OTA_TASK", 8192, NULL, 1, NULL, 0);
  }
}

// ------------------------------------------------------------------
//         FONCTION : Connexion MQTT (une fois le Wi-Fi OK)
// ------------------------------------------------------------------
bool connectToMQTT() {
  if (!wifiConnected) return false;

  espClient.setInsecure();
  client.setServer(mqttServer, mqttPort);
  client.setBufferSize(1024);
  Serial.print("Connexion au broker MQTT...");
  if (client.connect("ESP32Client", mqttUser, mqttPassword)) {
    Serial.println(" Connecté au MQTT !");
    client.setCallback(mqttCallback);
    String otaTopic = "breezly/devices/" + sensorId + "/ota";
    client.subscribe(otaTopic.c_str());
    Serial.printf("MQTT subscribed: %s\n", otaTopic.c_str());

    return true;
  } else {
    Serial.print(" Échec, état = ");
    Serial.println(client.state());
    return false;
  }
}
bool httpDownloadToUpdate(const String& binUrl) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0); // 0 = SHA-256
  uint8_t digest[32];


  WiFiClientSecure wcs;
  wcs.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);

  Serial.printf("[OTA] Téléchargement: %s\n", binUrl.c_str());
  if (!http.begin(wcs, binUrl)) { Serial.println("[OTA] http.begin() FAILED"); return false; }

  int code = http.GET();
  if (code != HTTP_CODE_OK) { Serial.printf("[OTA] HTTP code=%d\n", code); http.end(); return false; }

  int total = http.getSize();
  WiFiClient *stream = http.getStreamPtr();

  if (!Update.begin(total > 0 ? total : UPDATE_SIZE_UNKNOWN)) {
    Serial.printf("[OTA] Update.begin err=%u\n", Update.getError());
    http.end();
    return false;
  }

  // --- buffer sur le heap, pas sur la pile ---
  const size_t CHUNK = 2048; // 1-2 KB suffit
  uint8_t *buf = (uint8_t*)heap_caps_malloc(CHUNK, MALLOC_CAP_8BIT);
  if (!buf) { Serial.println("[OTA] malloc buffer FAILED"); http.end(); Update.end(); return false; }

  size_t written = 0;
  unsigned long lastLog = 0;

  while (http.connected() && (total > 0 || total == -1)) {
    size_t avail = stream->available();
    if (avail) {
      int toRead = avail > CHUNK ? CHUNK : avail;
      int c = stream->readBytes(buf, toRead);
      mbedtls_sha256_update_ret(&ctx, buf, c);
      if (c > 0) {
        size_t w = Update.write(buf, c);
        if (w != (size_t)c) {
          Serial.printf("[OTA] Update.write err=%u\n", Update.getError());
          free(buf);
          http.end();
          return false;
        }
        written += c;
        if (total > 0) total -= c;
      }
      esp_task_wdt_reset();
      client.loop();
      vTaskDelay(1);
    } else {
      vTaskDelay(1);
    }

    if (millis() - lastLog > 2000 && http.getSize() > 0) {
      lastLog = millis();
      int sizeKnown = http.getSize();
      int done = (int)written;
      int pct  = (int)((done * 100LL) / (sizeKnown > 0 ? sizeKnown : 1));
      Serial.printf("[OTA] %d%% (%u/%u)\n", pct, (unsigned)done, (unsigned)sizeKnown);
    }
  }

  free(buf);
  http.end();

  if (!Update.end()) { Serial.printf("[OTA] Update.end err=%u\n", Update.getError()); return false; }
  mbedtls_sha256_finish_ret(&ctx, digest);
  if (!Update.isFinished()) { Serial.println("[OTA] Update not finished"); return false; }
  bool shaOk = true;
for (int i=0;i<32;i++) {
  char byteHex[3]; sprintf(byteHex, "%02x", digest[i]);
  // if (strncasecmp(byteHex, &manifestShaHex[i*2], 2) != 0) { shaOk = false; break; } A METTRE
}
if (!shaOk) { Serial.println("[OTA] SHA256 mismatch, abort"); return false; }
  Serial.printf("[OTA] OK, écrit %u bytes. Reboot...\n", (unsigned)written);
  delay(250);
  ESP.restart();
  return true; // pas atteint
}


void checkAndPerformCloudOTA() {
  otaInProgress = true;
  if (WiFi.status() != WL_CONNECTED) { Serial.println("[OTA] WiFi KO"); otaInProgress = false; return; }

  WiFiClientSecure wcs;
  wcs.setInsecure(); // en prod: setCACert
  HTTPClient http;

  // --- cache-buster ---
  String url = String(FW_MANIFEST_URL) + "?t=" + String(millis());

  Serial.print("[OTA] GET manifest: "); Serial.println(url);
  if (!http.begin(wcs, url)) { Serial.println("[OTA] http.begin FAILED"); otaInProgress = false; return; }

  // pour éviter les encodages bizarres
  http.addHeader("Accept-Encoding", "identity");
  http.setReuse(false);       // ferme la socket après
  http.useHTTP10(true);       // pas de chunked (souvent + simple)

  int code = http.GET();
  Serial.printf("[OTA] HTTP code=%d\n", code);
  if (code != HTTP_CODE_OK) { http.end(); otaInProgress = false; return; }

  // Lire TOUT le corps en string (manifest petit)
  String body = http.getString();
  http.end();

  // Debug: montrer ce qu'on a reçu
  Serial.println("[OTA] Manifest body (first 300):");
  Serial.println(body.substring(0, 300));

  // Parser sans filtre (document petit)
  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("[OTA] JSON parse error: %s\n", err.c_str());
    otaInProgress = false;
    return;
  }

  const char* version = doc["version"] | "";
  const char* fwurl   = doc["url"]     | "";

  Serial.printf("[OTA] Parsed version='%s' url='%s'\n", version, fwurl);

  if (!version[0]) { Serial.println("[OTA] Champ 'version' absent"); otaInProgress = false; return; }
  if (String(version) == String(CURRENT_FIRMWARE_VERSION)) { Serial.println("[OTA] Déjà à jour"); otaInProgress = false; return; }
  if (!fwurl[0]) { Serial.println("[OTA] URL manquante dans manifest (après parse)"); otaInProgress = false; return; }

  Serial.printf("[OTA] New version %s (cur %s)\n", version, CURRENT_FIRMWARE_VERSION);
  httpDownloadToUpdate(String(fwurl));
  otaInProgress = false;
}





uint32_t computeChecksum(const String& ssid, const String& pwd, const String& sensor, const String& user) {
  String full = ssid + "|" + pwd + "|" + sensor + "|" + user;
  return esp_crc32_le(0, (const uint8_t*)full.c_str(), full.length());
}

bool isPreferencesValid() {
  prefs.begin("myApp", true);
  String ssid     = prefs.getString("wifiSSID", "");
  String pwd      = prefs.getString("wifiPassword", "");
  String sensorId = prefs.getString("sensorId", "");
  String userId   = prefs.getString("userId", "");
  uint32_t storedChecksum = prefs.getUInt("checksum", 0);
  prefs.end();

  uint32_t computed = computeChecksum(ssid, pwd, sensorId, userId);
  return storedChecksum == computed;
}

void ledTask(void *parameter);

// ------------------------------------------------------------------
//                          SETUP
// ------------------------------------------------------------------
void setup() {
  delay(4000);
  Serial.begin(115200);
  Serial.printf("Flash chip size: %u MB\n", ESP.getFlashChipSize() / (1024*1024));
  // Réinitialise le TWDT si déjà initialisé par le core

  // équivalent de idle_core_mask = 0b11
    esp_task_wdt_deinit();

  /*
  * Init WDT sans struct (API IDF4 / Arduino-ESP32 2.x)
  * - timeout = 30 s
  * - trigger_panic = true
  * - surveille la loopTask + les Idle tasks CPU0 et CPU1
  */
  esp_task_wdt_init(30, true);          // timeout en SECONDES, panic activé
  esp_task_wdt_add(NULL);               // surveille la loopTask


  Serial.print("Firmware version actuelle : ");
  Serial.println(CURRENT_FIRMWARE_VERSION);
  updateLedState(LED_BOOT);
  Serial.print("DEMARRAGE ! DEMARRAGE ! DEMARRAGE ! DEMARRAGE ! DEMARRAGE ! DEMARRAGE ! DEMARRAGE ! DEMARRAGE ! DEMARRAGE ! DEMARRAGE ! DEMARRAGE ! ");
  delay(1000);
  led.begin();
  Serial.print("début LED !");
  led.setBrightness(50); // 0 à 255
  
  led.show();
  xTaskCreatePinnedToCore(
    ledTask,
    "LED Task",
    8192,
    NULL,
    1,
    NULL,
    1  // ✅ Core 1 uniquement
  );

  Serial.print("FINITO LED !");
  // Mutex PMS
  gPmsMutex = xSemaphoreCreateMutex();

  // Lancer la task PMS sur Core 0 (LED est sur Core 1)
  xTaskCreatePinnedToCore(
    pmsTask,
    "PMS Task",
    4096,    // stack suffisant
    NULL,
    1,       // priorité basse (ne gêne pas MQTT/BLE)
    NULL,
    0        // Core 0
  );

  delay(1000);
  WiFi.disconnect(true);
  // Charger les identifiants depuis les préférences
  prefs.begin("myApp", true);
  wifiSSID  = prefs.getString("wifiSSID", "");
  wifiPassword = prefs.getString("wifiPassword", "");
  sensorId  = prefs.getString("sensorId", "");
  userId    = prefs.getString("userId", "");
  prefs.end();



bool valid = isPreferencesValid();
bool manqueIdentifiants = (wifiSSID.isEmpty() || wifiPassword.isEmpty());
bool doitFaireProvisioning = (!valid || manqueIdentifiants);

// Logs clairs
Serial.println("---------- VALIDATION -----------");
Serial.printf("WiFi SSID : %s\n", wifiSSID.c_str());
Serial.printf("WiFi Pwd  : %s\n", wifiPassword.c_str());
Serial.printf("Sensor ID : %s\n", sensorId.c_str());
Serial.printf("User ID   : %s\n", userId.c_str());
Serial.printf("valid              : %s\n", valid ? "true" : "false");
Serial.printf("manqueIdentifiants : %s\n", manqueIdentifiants ? "true" : "false");
Serial.printf("doitFaireProvisioning : %s\n", doitFaireProvisioning ? "true" : "false");
Serial.println("---------------------------------");
if (doitFaireProvisioning) {
  setupBLE(true);
} else {
  Serial.println("BLE SKIPPED: identifiants valides, pas de provisioning.");
}
  // 2) Tenter la connexion Wi-Fi immédiate si on a des identifiants
  if (!manqueIdentifiants) {
    Serial.println("Tentative de connexion avec les identifiants sauvegardés...");
    connectToWiFi();
    // Si échec => on active la pub BLE dans connectToWiFi()
  }
  Serial.println("après setupBLE");

if (doitFaireProvisioning) {
  updateLedState(LED_PAIRING);
  Serial.println("🕐 En attente du scan QR Code via BLE...");
  while (!wifiConnected) {
    delay(500);
    esp_task_wdt_reset();
  }
  Serial.println("✅ Wi-Fi connecté, provisioning terminé.");
}



  // 3) Initialisation des capteurs
  Wire.begin();
  delay(100);
  if (!aht.begin()) {
    Serial.println("AHT21 initialization failed!");
    // On continue quand même, mais le capteur ne fonctionnera pas
  } else {
    Serial.println("AHT21 initialisé avec succès.");
  }
  
  if (!ens160.begin()) {
    Serial.println("Échec ENS160 !");
    //while (1) delay(10); // Bloque si impossible d'initialiser
  } else {
    Serial.println("ENS160 initialisé avec succès.");
    ens160.setMode(ENS160_OPMODE_STD);
  }
  String bootReport = "{";
  bootReport += "\"boot\":true,";
  bootReport += "\"sensorId\":\"" + sensorId + "\",";
  bootReport += "\"firmwareVersion\":\"" + String(CURRENT_FIRMWARE_VERSION) + "\"";
  bootReport += "}";
  Serial.println("boot msg : ");
  Serial.println(bootReport);
  client.publish("capteurs/boot", bootReport.c_str());
  

}
bool safeSensorRead(float& tempC, float& humidity) {
  sensors_event_t eventHum, eventTemp;

  // Tenter la lecture
  aht.getEvent(&eventHum, &eventTemp);

  // Vérifie si les données sont valides (pas NaN ou extrêmes)
  if (isnan(eventTemp.temperature) || isnan(eventHum.relative_humidity)) {
    Serial.println("Erreur de lecture capteur : données invalides");
    return false;
  }

  tempC = eventTemp.temperature;
  humidity = eventHum.relative_humidity;
  return true;
}

// ------------------------------------------------------------------
//                            LOOP
// ------------------------------------------------------------------
void loop() {
  if (wifiConnected && !otaInProgress && lastOtaCheck == 0) {
    lastOtaCheck = millis();
    xTaskCreatePinnedToCore([](void*){
      checkAndPerformCloudOTA();
      vTaskDelete(NULL);
    }, "OTA_CHECK_TASK_BOOT", 16384, NULL, 1, NULL, 0);

  }

  if (!otaInProgress && wifiConnected) {
  unsigned long now = millis();
  if ((long)(now - lastOtaCheck) >= (long)OTA_CHECK_INTERVAL_MS) {
    lastOtaCheck = now;
    // Lancer dans un task pour ne pas bloquer la publication capteurs
    xTaskCreatePinnedToCore([](void*){
      checkAndPerformCloudOTA();
      vTaskDelete(NULL);
    }, "OTA_CHECK_TASK", 16384, NULL, 1, NULL, 0);
  }
}
  // Si on a reçu de nouveaux credentials => tenter connexion Wi-Fi
  if (needToConnectWiFi && !wifiConnected) {
    needToConnectWiFi = false;  // évite de déclencher plusieurs fois
    connectToWiFi();
  }

  // Si Wi-Fi OK => tenter de se (re)connecter à MQTT
  if (wifiConnected && !client.connected()) {
    connectToMQTT();
  }


  // Si MQTT est connecté => publier périodiquement
  if (client.connected()) {
  unsigned long now = millis();
  if ((long)(now - lastPublish) >= (long)publishInterval) {
    lastPublish = now;
    
    ledOverride = false;
    // Lecture AHT21
    float tempC, humidity;
    if (!safeSensorRead(tempC, humidity)) {
      // Capteur en erreur => ne rien publier
      Serial.println("not safe to read");
      updateLedState(LED_BAD);
      delay(1000);
      return;
    }

    if (humidity >= 40 && humidity <= 60) {
      updateLedState(LED_GOOD);
    } else if ((humidity >= 20 && humidity < 40) || (humidity > 60 && humidity <= 70)) {
      updateLedState(LED_MODERATE);
    } else {
      Serial.println("craignos");
      updateLedState(LED_BAD);
    }

// --- Lecture AHT21 déjà faite plus haut via safeSensorRead(...) ---

if (ens160.available()) {
  ens160.set_envdata(tempC, humidity);
  ens160.measure(true);
  int aqi  = ens160.getAQI();
  int tvoc = ens160.getTVOC();
  int eco2 = ens160.geteCO2();

  // Snapshot PMS (protégé)
  PmsData p;
  bool havePms = false;
  if (xSemaphoreTake(gPmsMutex, 5 / portTICK_PERIOD_MS) == pdTRUE) {
    p = gPms;
    xSemaphoreGive(gPmsMutex);
    havePms = p.valid && (millis() - p.lastMs < 5000); // récent <5s
  }
  // Construire le JSON
  String message = "{";
  message += "\"temperature\":";  message += tempC;      message += ",";
  message += "\"humidity\":";     message += humidity;   message += ",";
  message += "\"AQI\":";          message += aqi;        message += ",";
  message += "\"TVOC\":";         message += tvoc;       message += ",";
  message += "\"eCO2\":";         message += eco2;       message += ",";

  // PMS (uniquement si on a une frame valide récente)
  if (havePms) {
    message += "\"pms\":{";

    // masses ATM
    message += "\"atm\":{";
    message += "\"pm1\":";  message += p.pm1_atm;  message += ",";
    message += "\"pm25\":"; message += p.pm25_atm; message += ",";
    message += "\"pm10\":"; message += p.pm10_atm; 
    message += "},";

    // masses CF1
    message += "\"cf1\":{";
    message += "\"pm1\":";  message += p.pm1_cf1;  message += ",";
    message += "\"pm25\":"; message += p.pm25_cf1; message += ",";
    message += "\"pm10\":"; message += p.pm10_cf1; 
    message += "},";

    // counts
    message += "\"counts\":{";
    message += "\"gt03\":";  message += p.gt03;   message += ",";
    message += "\"gt05\":";  message += p.gt05;   message += ",";
    message += "\"gt10\":";  message += p.gt10;   message += ",";
    message += "\"gt25\":";  message += p.gt25;   message += ",";
    message += "\"gt50\":";  message += p.gt50;   message += ",";
    message += "\"gt100\":"; message += p.gt100;
    message += "}";

    message += "},";
  }

  // IDs
  message += "\"sensorId\":\"";  message += sensorId;  message += "\",";
  message += "\"userId\":\"";    message += userId;    message += "\"";
  message += "}";

  client.publish("capteurs/qualite_air", message.c_str());
  Serial.println("Données publiées : " + message);
} else {
  Serial.println("ENS160 non disponible ou en attente de mesure.");
}

    // Maintenir la connexion au broker
    client.loop();
  
  }

  } else {
    // Pas de Wi-Fi ou pas de MQTT => on attend un peu
    delay(500);
  }
  esp_task_wdt_reset();  // Indique que tout va bien

}
void ledTask(void *parameter) {
  int brightness = 0;
  bool up = true;

  while (true) {
    switch (currentLedMode) {
      case LED_BOOT:
        led.setPixelColor(0, led.Color(0, 0, 150)); // Bleu fixe
        led.show();
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        break;

      case LED_PAIRING:
        led.setPixelColor(0, led.Color(0, 0, 150)); // Bleu clignotant
        led.show();
        vTaskDelay(300 / portTICK_PERIOD_MS);
        led.setPixelColor(0, 0); // off
        led.show();
        vTaskDelay(300 / portTICK_PERIOD_MS);
        break;

      case LED_GOOD:
        led.setPixelColor(0, led.Color(0, brightness, 0)); // Vert
        led.show();
        break;

      case LED_MODERATE:
        led.setPixelColor(0, led.Color(brightness, brightness, 0)); // Jaune
        led.show();
        break;

      case LED_BAD:
        led.setPixelColor(0, led.Color(255, 0, 0));
        led.show();
        break;
    }

      if (currentLedMode == LED_GOOD || currentLedMode == LED_MODERATE) {
        brightness += (up ? 5 : -5);
        if (brightness >= 150) { brightness = 150; up = false; }
        if (brightness <= 10)  { brightness = 10;  up = true; }
        vTaskDelay(30 / portTICK_PERIOD_MS);
      } else if (currentLedMode == LED_BAD) {
        brightness = 255; // Rouge fixe
        up = true; // réinitialise au cas où
        vTaskDelay(100 / portTICK_PERIOD_MS); // petit délai pour éviter surcharge CPU
      }
  }
}
void reconnectionTask(void *parameter) {
  for (;;) {
    // Si non connecté au Wi-Fi => tentative
    if (!wifiConnected) {
      Serial.println("[TASK] Wi-Fi non connecté, tentative de reconnexion...");
      connectToWiFi();
    }

    // Si Wi-Fi OK mais MQTT KO => tentative
    if (wifiConnected && !client.connected()) {
      Serial.println("[TASK] MQTT non connecté, tentative de reconnexion...");
      connectToMQTT();
    }

    vTaskDelay(10000 / portTICK_PERIOD_MS); // toutes les 10s
  }
}
void pmsTask(void *parameter) {
  PMS.begin(9600, SERIAL_8N1, PMS_RX, PMS_TX);
  uint32_t t0 = millis();
  while (millis() - t0 < 5000) vTaskDelay(100 / portTICK_PERIOD_MS);

  PmsData tmp;
  static uint32_t seqCounter = 0;
  for (;;) {
    if (readPmsFrame(PMS, tmp)) {
      tmp.seq = ++seqCounter;
      if (gPmsMutex && xSemaphoreTake(gPmsMutex, 5 / portTICK_PERIOD_MS) == pdTRUE) {
        gPms = tmp;
        xSemaphoreGive(gPmsMutex);
      }
    }
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}


