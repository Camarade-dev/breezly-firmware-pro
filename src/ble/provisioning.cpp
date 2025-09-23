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

class CredentialsCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    static String acc;
    std::string raw = c->getValue();
    acc += String(raw.c_str());

    // Attente de la fin JSON simple (fermeture “}”)
    if (!acc.endsWith("}")) return;

    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, acc);
    acc = ""; // reset buffer

    if (err) {
      if (statusCharacteristic){
        statusCharacteristic->setValue("{\"status\":\"json_error\"}");
        statusCharacteristic->notify();
      }
      return;
    }

    const char* ssid = doc["ssid"];
    const char* pwd  = doc["password"];
    const char* sId  = doc["sensorId"];
    const char* uId  = doc["userId"];

    if (!ssid || !pwd) {
      if (statusCharacteristic){
        statusCharacteristic->setValue("{\"status\":\"missing_fields\"}");
        statusCharacteristic->notify();
      }
      return;
    }

    // Mise à jour des globals (déclarés dans core/globals.h)
    wifiSSID     = String(ssid);
    wifiPassword = String(pwd);
    if (sId) sensorId = String(sId);
    if (uId) userId   = String(uId);

    // Persistance + checksum (identique à ton mono-fichier)
    uint32_t checksum = computeChecksum(wifiSSID, wifiPassword, sensorId, userId);
    prefs.begin("myApp", false);
    prefs.putString("wifiSSID", wifiSSID);
    prefs.putString("wifiPassword", wifiPassword);
    prefs.putString("sensorId",  sensorId);
    prefs.putString("userId",    userId);
    prefs.putUInt  ("checksum",  checksum);
    prefs.end();

    needToConnectWiFi = true;

    if (statusCharacteristic){
      statusCharacteristic->setValue("{\"status\":\"credentials_ok\"}");
      statusCharacteristic->notify();
    }
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
  NimBLEDevice::setMTU(256);
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
