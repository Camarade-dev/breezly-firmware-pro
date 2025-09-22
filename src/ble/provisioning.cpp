#include "provisioning.h"
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "../core/globals.h"
#include "../utils/crc_utils.h"

static BLEUUID serviceUUID("60f8a11f-e56f-4c3c-9658-5578e2f8d754");
static BLEUUID credUUID   ("d6f37ab1-4367-4001-b311-f219e161b736");
static BLEUUID statUUID   ("11f47ab2-1111-4002-b316-f219e161b736");

static NimBLECharacteristic* statChar = 0;

static void configureAdvertising(const String& name){
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising(); if (!adv) return;
  NimBLEAdvertisementData a; a.setFlags(0x06); a.setName(name.c_str()); a.addServiceUUID(serviceUUID);
  adv->setAdvertisementData(a);
  NimBLEAdvertisementData s; s.setName(name.c_str());
  adv->setScanResponseData(s);
}

class CredCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* ch, NimBLEConnInfo&) override {
    std::string v = ch->getValue();
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, v);
    if (err){
      Serial.print("[BLE] JSON error: "); Serial.println(err.c_str());
      if (statChar){ statChar->setValue("{\"status\":\"json_error\"}"); statChar->notify(); }
      return;
    }
    const char* ssid = doc["ssid"]     | (const char*)nullptr;
    const char* pwd  = doc["password"] | (const char*)nullptr;
    const char* sId  = doc["sensorId"] | (const char*)nullptr;
    const char* uId  = doc["userId"]   | (const char*)nullptr;

    if (!ssid || !pwd){
      Serial.println("[BLE] missing ssid/pwd");
      if (statChar){ statChar->setValue("{\"status\":\"missing_fields\"}"); statChar->notify(); }
      return;
    }

    wifiSSID = ssid; wifiPassword = pwd;
    if (sId) sensorId = sId;
    if (uId) userId   = uId;

    uint32_t cks = computeChecksum(wifiSSID, wifiPassword, sensorId, userId);
    prefs.begin("myApp", false);
    prefs.putString("wifiSSID", wifiSSID);
    prefs.putString("wifiPassword", wifiPassword);
    prefs.putString("sensorId", sensorId);
    prefs.putString("userId", userId);
    prefs.putUInt("checksum", cks);
    prefs.end();

    needToConnectWiFi = true;
    if (statChar){ statChar->setValue("{\"status\":\"credentials_ok\"}"); statChar->notify(); }
  }
};

void setupBLE(bool startAdv){
  prefs.begin("myApp", true);
  String name = prefs.getString("bleName", "");
  prefs.end();
  if (name.length()==0){
    char n[24]; uint64_t chip = ESP.getEfuseMac(); sprintf(n,"PROV_%012llX", chip); name = n;
    prefs.begin("myApp", false); prefs.putString("bleName", name); prefs.end();
  }

  NimBLEDevice::init(name.c_str());
  NimBLEDevice::setMTU(185);
  NimBLEDevice::setDeviceName(name.c_str());

  NimBLEServer* srv = NimBLEDevice::createServer();
  NimBLEService* svc = srv->createService(serviceUUID);

  NimBLECharacteristic* cred = svc->createCharacteristic(credUUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  cred->setCallbacks(new CredCb());

  statChar = svc->createCharacteristic(statUUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  statChar->setValue("{\"status\":\"not_connected\"}");

  svc->start();
  vTaskDelay(250/portTICK_PERIOD_MS);
  configureAdvertising(name);
  if (startAdv){ NimBLEDevice::getAdvertising()->start(); Serial.printf("BLE adv start (%s)\n", name.c_str()); }
  else          Serial.println("BLE ready (no adv)");
}

void restartBLEAdvertising(){
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising(); if (!adv) return;
  adv->stop(); vTaskDelay(50/portTICK_PERIOD_MS); adv->start();
  Serial.println("BLE advertising restarted");
}
