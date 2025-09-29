#include "provisioning.h"
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#include "core/globals.h"      // wifiSSID, wifiPassword, sensorId, userId, prefs, needToConnectWiFi
#include "utils/crc_utils.h"   // computeChecksum(...)

static NimBLEUUID serviceUUID("60f8a11f-e56f-4c3c-9658-5578e2f8d754");
static NimBLEUUID credentialsCharacteristicUUID("d6f37ab1-4367-4001-b311-f219e161b736");
static NimBLEUUID statusCharacteristicUUID      ("11f47ab2-1111-4002-b316-f219e161b736");

static NimBLECharacteristic* credentialsCharacteristic = nullptr;
static NimBLECharacteristic* statusCharacteristic      = nullptr;
static NimBLEServer*         pServer                   = nullptr;

static String gBleName;
static bool s_bleInitDone   = false;
static bool s_advertising   = false;

extern bool bleInited;  // exporté via header
// --- ajoute ce helper en haut du fichier ---


// --- remplace provisioningSetStatus par ---
void provisioningSetStatus(const char* json){
  if (statusCharacteristic){
    statusCharacteristic->setValue(json);
    statusCharacteristic->notify();
  }
}

void provisioningNotifyConnected(){
  provisioningSetStatus("{\"status\":\"connected\"}");
}

// --- Helpers -----------------------------------------------------------------
static void configureAdvertising() {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (!adv) return;

  NimBLEAdvertisementData advData;
  advData.setFlags(0x06);
  advData.setName(gBleName.c_str());
  advData.addServiceUUID(serviceUUID);
  adv->setAdvertisementData(advData);

  NimBLEAdvertisementData scanData;
  scanData.setName(gBleName.c_str());
  adv->setScanResponseData(scanData);
}

// provisioning.cpp (extrait)
static String g_acc;
static TaskHandle_t sCredTask = nullptr;
static SemaphoreHandle_t sAccMutex;
static void credWorker(void*){
    for(;;){
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    String json;
    xSemaphoreTake(sAccMutex, portMAX_DELAY);
    json = g_acc;
    g_acc = "";
    xSemaphoreGive(sAccMutex);

    // parse + NVS + logs ICI (pas dans onWrite)
    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, json);
    if (err){ provisioningSetStatus("{\"status\":\"json_error\"}"); continue; }

    String modeS = doc["mode"]     | "psk";
    String ssidS = doc["ssid"]     | "";
    String sIdS  = doc["sensorId"] | "";
    String uIdS  = doc["userId"]   | "";

    if (modeS.equalsIgnoreCase("eap")) {
      String userS = doc["username"]  | "";
      String passS = doc["password"]  | "";
      String idS   = doc["identity"]  | userS;
      String anonS = doc["anonymous"] | "";

      if (ssidS.isEmpty() || userS.isEmpty() || passS.isEmpty()){
        provisioningSetStatus("{\"status\":\"missing_fields\"}");
        continue;
      }

      wifiSSID=ssidS; wifiAuthType=WIFI_CONN_EAP_PEAP_MSCHAPV2;
      eapUsername=userS; eapPassword=passS; eapIdentity=idS; eapAnon=anonS;
      if (!sIdS.isEmpty()) sensorId=sIdS;
      if (!uIdS.isEmpty()) userId=uIdS;

      uint32_t checksum=computeChecksum(wifiSSID,String(""),sensorId,userId);
      prefs.begin("myApp", false);
      prefs.putString("wifiSSID", wifiSSID);
      prefs.putUInt  ("wifiAuthType",(uint32_t)wifiAuthType);
      prefs.putString("eapUsername",  eapUsername);
      prefs.putString("eapPassword",  eapPassword);
      prefs.putString("eapIdentity",  eapIdentity);
      prefs.putString("eapAnon",      eapAnon);
      prefs.putString("sensorId",     sensorId);
      prefs.putString("userId",       userId);
      prefs.putUInt  ("checksum",     checksum);
      prefs.end();
    } else {
      String pwdS = doc["password"] | "";
      if (ssidS.isEmpty() || pwdS.isEmpty()){
        provisioningSetStatus("{\"status\":\"missing_fields\"}");
        continue;
      }
      wifiSSID=ssidS; wifiPassword=pwdS; wifiAuthType=WIFI_CONN_PSK;
      if (!sIdS.isEmpty()) sensorId=sIdS;
      if (!uIdS.isEmpty()) userId=uIdS;

      uint32_t checksum=computeChecksum(wifiSSID,wifiPassword,sensorId,userId);
      prefs.begin("myApp", false);
      prefs.putString("wifiSSID",     wifiSSID);
      prefs.putString("wifiPassword", wifiPassword);
      prefs.putUInt  ("wifiAuthType", (uint32_t)wifiAuthType);
      prefs.putString("sensorId",     sensorId);
      prefs.putString("userId",       userId);
      prefs.putUInt  ("checksum",     checksum);
      prefs.end();
    }

    needToConnectWiFi = true;
    provisioningSetStatus("{\"status\":\"connecting\"}");
  }
}

class CredentialsCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    const std::string raw = c->getValue();
    if (!sAccMutex) sAccMutex = xSemaphoreCreateMutex();
    xSemaphoreTake(sAccMutex, portMAX_DELAY);
    g_acc += String(raw.c_str());
    bool done = g_acc.endsWith("}");
    xSemaphoreGive(sAccMutex);
    if (!done) return;

    if (!sCredTask){
      xTaskCreatePinnedToCore(credWorker, "BLE_CRED", 8192, nullptr, 1, &sCredTask, 0);
    }
    xTaskNotifyGive(sCredTask);
  }
};

// --- API ---------------------------------------------------------------------
void setupBLE(bool startAdvertising) {
  // Anti double-init : si déjà init, on peut juste (re)lancer la pub.
  if (s_bleInitDone) {
    if (startAdvertising && !s_advertising) {
      configureAdvertising();
      NimBLEDevice::startAdvertising();
      s_advertising = NimBLEDevice::getAdvertising()->isAdvertising();
      Serial.printf("[BLE] re-entry startAdvertising -> %d\n", (int)s_advertising);
    }
    return;
  }

  // 1) Nom BLE persistant comme dans ton code qui marche
  char bleName[25];
  prefs.begin("myApp", true);
  String storedBleName = prefs.getString("bleName", "");
  prefs.end();

  if (storedBleName.isEmpty()) {
    uint64_t chipid = ESP.getEfuseMac();
    sprintf(bleName, "PROV_%012llX", chipid);
    prefs.begin("myApp", false);
    prefs.putString("bleName", bleName);
    prefs.end();
  } else {
    storedBleName.toCharArray(bleName, sizeof(bleName));
  }
  gBleName = String(bleName);

  // ⚠️ Laisse la coexistence Wi-Fi/BLE gérer : NE PAS couper le Wi-Fi ici.
  //    Pas de esp_bt_controller_mem_release() non plus. (C’était la cause la plus probable.)

  // 2) Init NimBLE identique à la version monolithique
  Serial.printf("[BLE] INIT NimBLE (%s)\n", bleName);
  NimBLEDevice::init(bleName);
  NimBLEDevice::setMTU(185);
  NimBLEDevice::setDeviceName(bleName);

  pServer = NimBLEDevice::createServer();

  NimBLEService* pService = pServer->createService(serviceUUID);

  credentialsCharacteristic = pService->createCharacteristic(
    credentialsCharacteristicUUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  credentialsCharacteristic->setCallbacks(new CredentialsCallback());
  credentialsCharacteristic->createDescriptor("2901")->setValue("WiFi Credentials");

  statusCharacteristic = pService->createCharacteristic(
    statusCharacteristicUUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );
  statusCharacteristic->createDescriptor("2901")->setValue("Device Status");
  statusCharacteristic->setValue("{\"status\":\"not_connected\"}");

  pService->start();             // <- IMPORTANT : start le service AVANT de configurer la pub
  delay(50);

  configureAdvertising();        // payload (nom + service UUID)
  delay(20);

  if (startAdvertising) {
    NimBLEDevice::startAdvertising();   // appel “haut niveau” fiable
    s_advertising = NimBLEDevice::getAdvertising()->isAdvertising();
    Serial.printf("[BLE] advertising=%d\n", (int)s_advertising);
  } else {
    Serial.println("[BLE] Ready (no advertising)");
  }

  s_bleInitDone = true;
  bleInited     = true;
}

void restartBLEAdvertising() {
  if (!s_bleInitDone) return;         // pas d’init -> pas de restart
  NimBLEAdvertising* a = NimBLEDevice::getAdvertising();
  if (!a) return;

  configureAdvertising();
  a->stop();
  delay(80);
  NimBLEDevice::startAdvertising();
  s_advertising = a->isAdvertising();
  Serial.printf("[BLE] restart -> advertising=%d\n", (int)s_advertising);
}
