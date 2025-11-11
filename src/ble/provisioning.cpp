#include "provisioning.h"
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#include "core/globals.h"      // wifiSSID, wifiPassword, sensorId, userId, prefs, needToConnectWiFi
#include "utils/crc_utils.h"   // computeChecksum(...)
#include "mbedtls/md.h"
#include "core/devkey_runtime.h"
#include "led/led_status.h"   // ledSuspend()/ledResume()
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
static volatile uint32_t s_lastActivityMs = 0;
static TaskHandle_t sWatchdogTask = nullptr;
static bool s_wdEnabled = false;          // <— nouveau

// 3 minutes de grâce par défaut (ajuste si tu veux)
#ifndef PROV_IDLE_TIMEOUT_MS
#define PROV_IDLE_TIMEOUT_MS 15000UL  // 15 s pour tester
#endif
static inline void wd_kick() { s_lastActivityMs = millis(); }
static volatile bool s_gattConnected = false;   // NEW
static inline void wd_enable(bool on) {
  s_wdEnabled = on;
  if (on) wd_kick();
}
static bool hmac_sha256(const uint8_t* key, size_t keylen, const uint8_t* msg, size_t msglen, uint8_t out[32]){
  mbedtls_md_context_t ctx; mbedtls_md_init(&ctx);
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (mbedtls_md_setup(&ctx, info, 1) != 0) { mbedtls_md_free(&ctx); return false; }
  if (mbedtls_md_hmac_starts(&ctx, key, keylen) != 0) { mbedtls_md_free(&ctx); return false; }
  if (mbedtls_md_hmac_update(&ctx, msg, msglen) != 0) { mbedtls_md_free(&ctx); return false; }
  if (mbedtls_md_hmac_finish(&ctx, out) != 0) { mbedtls_md_free(&ctx); return false; }
  mbedtls_md_free(&ctx);
  return true;
}

static String base64Encode(const uint8_t* buf, size_t len){
  // Arduino base64: use extern or write minimal encoder; ici on s'appuie sur Arduino's built-in
  // Si pas dispo chez toi, remplace par ton encodeur existant (tu en as déjà côté JS).
  String out; out.reserve(((len+2)/3)*4);
  static const char* tbl="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for(size_t i=0;i<len;i+=3){
    uint32_t v = (buf[i]<<16) | ((i+1<len?buf[i+1]:0)<<8) | (i+2<len?buf[i+2]:0);
    out += tbl[(v>>18)&63]; out += tbl[(v>>12)&63];
    out += (i+1<len)?tbl[(v>>6)&63] : '=';
    out += (i+2<len)?tbl[v&63] : '=';
  }
  return out;
}
// --- remplace provisioningSetStatus par ---
void provisioningSetStatus(const char* json){
  if (statusCharacteristic){
    // LOG sortant vers l'app
    Serial.printf("[ESP->APP][NOTIFY] JSON len=%u\n", (unsigned)strlen(json));
    Serial.printf("[ESP->APP][NOTIFY] %s\n", json);
    statusCharacteristic->setValue(json);
    statusCharacteristic->notify();
  } else {
    Serial.println("[ESP->APP][NOTIFY] statusCharacteristic NULL");
  }
}

void provisioningNotifyConnected(){
  provisioningSetStatus("{\"status\":\"connected\"}");
  wd_enable(false);
  s_gattConnected = false;  // on coupe la session BLE “logiquement”
  Serial.println("[WD] provisioning complete -> WD disabled, gatt=0");
}
// === Helpers de log hex/ASCII ===
static void printHex(const uint8_t* buf, size_t len, const char* tag = "HEX") {
  Serial.printf("[BLE][%s] len=%u : ", tag, (unsigned)len);
  for (size_t i = 0; i < len; ++i) Serial.printf("%02X", buf[i]);
  Serial.println();
}

static void printAsciiPreview(const uint8_t* buf, size_t len, size_t maxShow = 80) {
  Serial.print("[BLE][ASCII] ");
  size_t show = len < maxShow ? len : maxShow;
  for (size_t i = 0; i < show; ++i) {
    char c = (char)buf[i];
    if (c >= 32 && c < 127) Serial.write(c); else Serial.write('.');
  }
  if (len > show) Serial.print("…");
  Serial.println();
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
    Serial.println("[BLE][WORKER] wake: processing accumulated credentials/ops…");

    String json;
    xSemaphoreTake(sAccMutex, portMAX_DELAY);
    json = g_acc;
    g_acc = "";
    xSemaphoreGive(sAccMutex);

    Serial.printf("[BLE][WORKER] JSON total len=%u\n", (unsigned)json.length());
    Serial.printf("[BLE][WORKER] JSON payload: %s\n", json.c_str());

    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, json);
    if (err){
      Serial.printf("[BLE][WORKER][ERROR] JSON deserialization: %s\n", err.f_str());
      provisioningSetStatus("{\"status\":\"json_error\"}");
      continue;
    }

    const char* op = doc["op"] | "";
    if (strcmp(op, "abort") == 0) {
      // Feedback à l’app + remise à zéro de l’état de provisioning
      provisioningSetStatus("{\"status\":\"aborted\",\"reason\":\"client_abort\"}");
      g_acc = "";  // purge JSON accumulé
      // Déconnexion “propre” si encore connecté côté GATT
      NimBLEDevice::stopAdvertising(); // (facultatif, on va relancer)
      // Si tu as un handle, tu peux forcer la déconnexion. Sinon, redémarrer la pub suffit.
      restartBLEAdvertising();
      continue; // rien d’autre à faire
    }

    Serial.printf("[BLE][WORKER] op=\"%s\"\n", op);

    String modeS = doc["mode"]     | "psk";
    String ssidS = doc["ssid"]     | "";
    String sIdS  = doc["sensorId"] | "";
    String uIdS  = doc["userId"]   | "";

    if (strcmp(op, "claim_challenge") == 0) {
      String nonceB64 = doc["nonce"] | "";
      uint32_t counter = doc["counter"] | 0;
      Serial.printf("[BLE][WORKER] claim_challenge: nonceB64.len=%u counter=%u\n",
                    (unsigned)nonceB64.length(), counter);
      
      if (g_deviceKeyB64.length() == 0 || nonceB64.length() == 0) {
        Serial.println("[BLE][WORKER][ERROR] missing key or nonce");
        provisioningSetStatus("{\"op\":\"claim_proof\",\"err\":\"missing_key_or_nonce\"}");
        continue;
      }

      auto b64Dec = [](const String& s)->std::vector<uint8_t>{
        const char* tbl="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        auto idx=[&](char c)->int{ const char* p=strchr(tbl,c); return p? (int)(p-tbl) : (c=='='?-1:-2); };
        std::vector<uint8_t> out; out.reserve((s.length()*3)/4);
        int val=0, valb=-8;
        for (size_t i=0;i<s.length();++i){
          int c = idx(s[i]); if (c==-2) continue; if (c==-1) break;
          val=(val<<6)+c; valb+=6;
          if (valb>=0){ out.push_back((uint8_t)((val>>valb)&0xFF)); valb-=8; }
        }
        return out;
      };

      std::vector<uint8_t> nonce = b64Dec(nonceB64);
      Serial.printf("[BLE][WORKER] decoded nonce len=%u (expected 32)\n", (unsigned)nonce.size());
      if (nonce.size() != 32) {
        Serial.println("[BLE][WORKER][ERROR] bad_nonce_len");
        provisioningSetStatus("{\"op\":\"claim_proof\",\"err\":\"bad_nonce_len\"}");
        continue;
      }

      uint8_t msg[36]; size_t mlen = 0;
      memcpy(msg, nonce.data(), 32); mlen = 32;
      msg[mlen+0] = (uint8_t)((counter>>24)&0xFF);
      msg[mlen+1] = (uint8_t)((counter>>16)&0xFF);
      msg[mlen+2] = (uint8_t)((counter>>8)&0xFF);
      msg[mlen+3] = (uint8_t)(counter&0xFF);
      mlen += 4;
      printHex(msg, mlen, "CLAIM_MSG");

      std::vector<uint8_t> key = b64Dec(g_deviceKeyB64);
      Serial.printf("[BLE][WORKER] decoded key len=%u\n", (unsigned)key.size());

      uint8_t mac[32];
      if (!hmac_sha256(key.data(), key.size(), msg, mlen, mac)) {
        Serial.println("[BLE][WORKER][ERROR] hmac_failed");
        provisioningSetStatus("{\"op\":\"claim_proof\",\"err\":\"hmac_failed\"}");
        continue;
      }
      String macB64 = base64Encode(mac, 32);
      Serial.printf("[BLE][WORKER] hmac(b64).len=%u\n", (unsigned)macB64.length());

      StaticJsonDocument<128> out;
      out["op"]   = "claim_proof";
      out["hmac"] = macB64;
      String s; serializeJson(out, s);
      provisioningSetStatus(s.c_str());
      continue;
    }

    // Logs généraux sur les credentials
    Serial.printf("[BLE][WORKER] mode=%s, ssid.len=%u, sensorId.len=%u, userId.len=%u\n",
                  modeS.c_str(), (unsigned)ssidS.length(), (unsigned)sIdS.length(), (unsigned)uIdS.length());

    if (modeS.equalsIgnoreCase("eap")) {
      String userS = doc["username"]  | "";
      String passS = doc["password"]  | "";
      String idS   = doc["identity"]  | userS;
      String anonS = doc["anonymous"] | "";
      Serial.printf("[BLE][WORKER] EAP: user.len=%u pass.len=%u id.len=%u anon.len=%u\n",
                    (unsigned)userS.length(), (unsigned)passS.length(), (unsigned)idS.length(), (unsigned)anonS.length());

      if (ssidS.isEmpty() || userS.isEmpty() || passS.isEmpty()){
        Serial.println("[BLE][WORKER][ERROR] missing_fields (EAP)");
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
      Serial.printf("[BLE][WORKER] PSK: pwd.len=%u\n", (unsigned)pwdS.length());
      if (ssidS.isEmpty() || pwdS.isEmpty()){
        Serial.println("[BLE][WORKER][ERROR] missing_fields (PSK)");
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
class ServerCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, ble_gap_conn_desc* d)  {
    s_gattConnected = true;                     // <— flag sûr
    wd_enable(true);
    provisioningSetStatus("{\"status\":\"ble_connected\"}");
    Serial.println("[WD] onConnect: enable WD, gattConnected=1");
  }
  void onDisconnect(NimBLEServer* s, ble_gap_conn_desc* d)  {
    s_gattConnected = false;                    // <— flag sûr
    provisioningSetStatus("{\"status\":\"idle\"}");
    g_acc = "";
    wd_enable(false);
    Serial.println("[WD] onDisconnect: disable WD, gattConnected=0");
    restartBLEAdvertising();
  }
};


class CredentialsCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    wd_kick();
    const std::string raw = c->getValue();
    Serial.printf("[APP->ESP][WRITE] chunk bytes=%u\n", (unsigned)raw.length());
    if (!raw.empty()) {
      printHex((const uint8_t*)raw.data(), raw.size(), "APP->ESP");
      printAsciiPreview((const uint8_t*)raw.data(), raw.size());
    }

    if (!sAccMutex) sAccMutex = xSemaphoreCreateMutex();
    xSemaphoreTake(sAccMutex, portMAX_DELAY);

    g_acc.reserve(g_acc.length() + raw.length());
    for (size_t i = 0; i < raw.length(); ++i) g_acc += (char)raw[i];

    Serial.printf("[BLE][onWrite] accLen=%u\n", (unsigned)g_acc.length());
    if (g_acc.length() >= 1) {
      char last = g_acc[g_acc.length()-1];
      Serial.printf("[BLE][onWrite] acc.last='%c' (0x%02X)\n",
        (last>=32 && last<127)?last:'.', (unsigned char)last);
    }

    bool done = g_acc.endsWith("}");
    if (done) {
      Serial.println("[BLE][onWrite] detected end-of-JSON '}' -> wake worker");
    }
    xSemaphoreGive(sAccMutex);
    if (!done) return;

    if (!sCredTask){
      xTaskCreatePinnedToCore(credWorker, "BLE_CRED", 8192, nullptr, 1, &sCredTask, 0);
    }
    xTaskNotifyGive(sCredTask);
  }
};
static void sessionWatchdog(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));

    if (!s_wdEnabled)                continue;
    if (!s_gattConnected)            continue;   // <— remplace getConnectedCount()
    // (optionnel) si tu veux garder un garde-fou : if (!pServer) continue;

    const uint32_t now  = millis();
    const uint32_t idle = now - s_lastActivityMs;

    // DEBUG : trace toutes les 5 s
    static uint32_t lastLog = 0;
    if (now - lastLog > 5000) {
      Serial.printf("[WD] enabled=1, gatt=1, idle=%lu/%lu ms\n",
                    (unsigned long)idle, (unsigned long)PROV_IDLE_TIMEOUT_MS);
      lastLog = now;
    }

    if (idle > PROV_IDLE_TIMEOUT_MS) {
      Serial.println("[WD] TIMEOUT -> notify+restart adv");
      // Ne notifie que si encore connecté (sinon ça ne sert à rien et peut planter)
      provisioningSetStatus("{\"status\":\"timeout\"}");
      g_acc = "";
      restartBLEAdvertising();
      wd_kick();               // évite une rafale
      // On garde s_wdEnabled= true : si l’app se reconnecte, on repart
    }
  }
}



// --- API ---------------------------------------------------------------------
void setupBLE(bool startAdvertising) {
  wd_kick();
  Serial.printf("[WD] init: timeout=%lu ms\n", (unsigned long)PROV_IDLE_TIMEOUT_MS);
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
  pServer->setCallbacks(new ServerCb());
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
  if (!sWatchdogTask) {
    xTaskCreatePinnedToCore(sessionWatchdog, "BLE_WD", 4096, nullptr, 1, &sWatchdogTask, 0);
  }
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
