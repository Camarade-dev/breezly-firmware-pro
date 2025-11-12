#include "provisioning.h"
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#include "core/globals.h"      // wifiSSID, wifiPassword, sensorId, userId, prefs, needToConnectWiFi
#include "utils/crc_utils.h"   // computeChecksum(...)
#include "mbedtls/md.h"
#include "core/devkey_runtime.h"
#include "led/led_status.h"    // ledSuspend()/ledResume()

static NimBLEUUID serviceUUID("60f8a11f-e56f-4c3c-9658-5578e2f8d754");
static NimBLEUUID credentialsCharacteristicUUID("d6f37ab1-4367-4001-b311-f219e161b736");
static NimBLEUUID statusCharacteristicUUID      ("11f47ab2-1111-4002-b316-f219e161b736");

static NimBLECharacteristic* credentialsCharacteristic = nullptr;
static NimBLECharacteristic* statusCharacteristic      = nullptr;
static NimBLEServer*         pServer                   = nullptr;

static String gBleName;
static bool s_bleInitDone = false;
static bool s_advertising = false;

extern bool bleInited;  // exporté via header

// ------------------- WD & session -------------------
static volatile uint32_t s_lastActivityMs = 0;
static TaskHandle_t sWatchdogTask = nullptr;
static bool s_wdEnabled = false;

#ifndef PROV_IDLE_TIMEOUT_MS
#define PROV_IDLE_TIMEOUT_MS 60000UL
#endif
#ifndef BLE_HS_CONN_HANDLE_NONE
#define BLE_HS_CONN_HANDLE_NONE 0xFFFF
#endif

static volatile bool s_gattConnected = false;
static uint16_t s_connHandle = BLE_HS_CONN_HANDLE_NONE;

// --- Session state -----------------------------------------------------------
enum class ProvPhase : uint8_t { IDLE, STARTING, SELECTING_SSID, TYPING, CLAIM, SENDING_CREDS, CONNECTING, DONE };

static volatile ProvPhase s_phase = ProvPhase::IDLE;
static volatile bool s_appForeground = true; // piloté par l’app
static bool s_warned = false;                // pré-warning émis ?

static inline void wd_kick(){ s_lastActivityMs = millis(); s_warned = false; }
static inline void setPhase(ProvPhase p){ s_phase = p; wd_kick(); }
static inline void setAppFg(bool fg){ s_appForeground = fg; wd_kick(); }

static inline void wd_enable(bool on){
  s_wdEnabled = on;
  if (on) wd_kick();
  Serial.printf("[WD] enable=%d\n", (int)on);
}

static uint32_t computePhaseTimeoutMs(){
  // Timeouts côté app au 1er plan
  uint32_t t_fg;
  switch (s_phase) {
    case ProvPhase::TYPING:          t_fg = 5UL*60*1000;  break; // 5 min
    case ProvPhase::SELECTING_SSID:  t_fg = 3UL*60*1000;  break; // 3 min
    case ProvPhase::CLAIM:           t_fg = 60UL*1000;    break; // 1 min
    case ProvPhase::SENDING_CREDS:   t_fg = 60UL*1000;    break;
    case ProvPhase::CONNECTING:      t_fg = 90UL*1000;    break; // 1m30
    case ProvPhase::STARTING:        t_fg = 60UL*1000;    break;
    case ProvPhase::DONE:            t_fg = 20UL*1000;    break;
    default:                         t_fg = 60UL*1000;    break;
  }
  if (s_appForeground) return t_fg;

  // App en arrière-plan : libérer vite l'appareil
  switch (s_phase) {
    case ProvPhase::TYPING:          return 30UL*1000;
    case ProvPhase::SELECTING_SSID:  return 30UL*1000;
    case ProvPhase::CLAIM:           return 15UL*1000;
    case ProvPhase::SENDING_CREDS:   return 20UL*1000;
    case ProvPhase::CONNECTING:      return 30UL*1000;
    default:                         return 20UL*1000;
  }
}

// ------------------- crypto & utils -------------------
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

// ------------------- notify helpers -------------------
void provisioningSetStatus(const char* json){
  if (statusCharacteristic){
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
  s_gattConnected = false;
  Serial.println("[WD] provisioning complete -> WD disabled, gatt=0");
}

// ------------------- advertising -------------------
static void configureAdvertising(){
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

// ------------------- worker JSON -------------------
static String g_acc;
static TaskHandle_t sCredTask = nullptr;
static SemaphoreHandle_t sAccMutex;

// NEW: hooks C appelables depuis ton code Wi-Fi/MQTT pour avertir l’app en temps réel.
void breezly_on_wifi_ok()            { provisioningSetStatus("{\"status\":\"wifi_ok\"}"); }
void breezly_on_wifi_auth_failed()   { provisioningSetStatus("{\"status\":\"wifi_auth_failed\"}"); }
void breezly_on_wifi_assoc_timeout() { provisioningSetStatus("{\"status\":\"wifi_assoc_timeout\"}"); }
void breezly_on_inet_ok()            { provisioningSetStatus("{\"status\":\"inet_ok\"}"); }
void breezly_on_mqtt_hello_ok()      { provisioningSetStatus("{\"status\":\"hello_ok\"}"); }
void breezly_on_registered()         { provisioningSetStatus("{\"status\":\"registered\"}"); }
void breezly_on_connected_final()    { provisioningSetStatus("{\"status\":\"connected\"}"); }

static void credWorker(void*){
  for(;;){
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    Serial.println("[BLE][WORKER] wake: processing accumulated credentials/ops…");

    String json;
    xSemaphoreTake(sAccMutex, portMAX_DELAY);
    json = g_acc; g_acc = "";
    xSemaphoreGive(sAccMutex);

    Serial.printf("[BLE][WORKER] JSON total len=%u\n", (unsigned)json.length());
    Serial.printf("[BLE][WORKER] JSON payload: %s\n", json.c_str());

    DynamicJsonDocument doc(1536);
    DeserializationError err = deserializeJson(doc, json);
    if (err){
      Serial.printf("[BLE][WORKER][ERROR] JSON deserialization: %s\n", err.f_str());
      provisioningSetStatus("{\"status\":\"json_error\"}");
      continue;
    }

    const char* op = doc["op"] | "";

    // --------- Annulation côté app ---------
    if (!strcmp(op, "abort")) {
      provisioningSetStatus("{\"status\":\"aborted\",\"reason\":\"client_abort\"}");
      g_acc = "";
      restartBLEAdvertising();
      continue;
    }

    // --------- Sync UI → device ---------
    if (!strcmp(op, "app_state")) {
      const char* v = doc["value"] | "fg";
      setAppFg(strcmp(v,"bg")!=0);
      provisioningSetStatus("{\"status\":\"app_state_ok\"}");
      continue;
    }
    if (!strcmp(op, "phase")) {
      const char* v = doc["value"] | "IDLE";
      if      (!strcmp(v,"STARTING"))       setPhase(ProvPhase::STARTING);
      else if (!strcmp(v,"SELECTING_SSID")) setPhase(ProvPhase::SELECTING_SSID);
      else if (!strcmp(v,"TYPING"))         setPhase(ProvPhase::TYPING);
      else if (!strcmp(v,"CLAIM"))          setPhase(ProvPhase::CLAIM);
      else if (!strcmp(v,"SENDING_CREDS"))  setPhase(ProvPhase::SENDING_CREDS);
      else if (!strcmp(v,"CONNECTING"))     setPhase(ProvPhase::CONNECTING);
      else if (!strcmp(v,"DONE"))           setPhase(ProvPhase::DONE);
      else                                   setPhase(ProvPhase::IDLE);
      provisioningSetStatus("{\"status\":\"phase_ok\"}");
      continue;
    }

    // --------- Maintenance ---------
    if (!strcmp(op, "erase")) {
      // Efface uniquement les creds Wi-Fi (pas les clés device)
      prefs.begin("myApp", false);
      prefs.remove("wifiSSID");
      prefs.remove("wifiPassword");
      prefs.remove("wifiAuthType");
      prefs.remove("eapUsername");
      prefs.remove("eapPassword");
      prefs.remove("eapIdentity");
      prefs.remove("eapAnon");
      prefs.remove("checksum");
      prefs.end();
      provisioningSetStatus("{\"status\":\"erased\"}");
      continue;
    }

    if (!strcmp(op, "factory_reset")) {
      // Efface creds + IDs utilisateur; garde l’ID matériel
      prefs.begin("myApp", false);
      prefs.clear(); // si tu veux vraiment tout virer du namespace "myApp"
      prefs.end();
      provisioningSetStatus("{\"status\":\"factory_done\"}");
      // Option: reboot doux pour revenir direct en advertising propre
      delay(100);
      ESP.restart();
      continue;
    }

    // --------- Claim HMAC ---------
    if (!strcmp(op, "claim_challenge")) {
      setPhase(ProvPhase::CLAIM);
      String nonceB64 = doc["nonce"] | "";
      uint32_t counter = doc["counter"] | 0;
      if (g_deviceKeyB64.length()==0 || nonceB64.length()==0) {
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
      auto nonce = b64Dec(nonceB64);
      if (nonce.size()!=32) {
        provisioningSetStatus("{\"op\":\"claim_proof\",\"err\":\"bad_nonce_len\"}");
        continue;
      }
      uint8_t msg[36]; memcpy(msg, nonce.data(), 32);
      msg[32]=(uint8_t)((counter>>24)&0xFF);
      msg[33]=(uint8_t)((counter>>16)&0xFF);
      msg[34]=(uint8_t)((counter>>8)&0xFF);
      msg[35]=(uint8_t)(counter&0xFF);

      auto key = b64Dec(g_deviceKeyB64);
      uint8_t mac[32];
      if (!hmac_sha256(key.data(), key.size(), msg, 36, mac)) {
        provisioningSetStatus("{\"op\":\"claim_proof\",\"err\":\"hmac_failed\"}");
        continue;
      }
      String macB64 = base64Encode(mac, 32);
      StaticJsonDocument<128> out; out["op"]="claim_proof"; out["hmac"]=macB64;
      String s; serializeJson(out, s);
      provisioningSetStatus(s.c_str());
      continue;
    }

    // --------- Credentials (EAP/PSK) ---------
    if (!strcmp(op, "provision")) {
      String modeS = doc["mode"]     | "psk";
      String ssidS = doc["ssid"]     | "";
      String sIdS  = doc["sensorId"] | "";
      String uIdS  = doc["userId"]   | "";

      setPhase(ProvPhase::SENDING_CREDS);

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
      } else {
        String pwdS = doc["password"] | "";
        if (ssidS.isEmpty() || pwdS.isEmpty()){
          provisioningSetStatus("{\"status\":\"missing_fields\"}");
          continue;
        }
        wifiSSID=ssidS; wifiPassword=pwdS; wifiAuthType=WIFI_CONN_PSK;
      }
      if (!sIdS.isEmpty()) sensorId=sIdS;
      if (!uIdS.isEmpty()) userId=uIdS;

      uint32_t checksum = computeChecksum(wifiSSID,(wifiAuthType==WIFI_CONN_PSK)?wifiPassword:String(""),sensorId,userId);
      prefs.begin("myApp", false);
      prefs.putString("wifiSSID",     wifiSSID);
      if (wifiAuthType==WIFI_CONN_PSK) prefs.putString("wifiPassword", wifiPassword);
      prefs.putUInt  ("wifiAuthType", (uint32_t)wifiAuthType);
      if (wifiAuthType==WIFI_CONN_EAP_PEAP_MSCHAPV2) {
        prefs.putString("eapUsername", eapUsername);
        prefs.putString("eapPassword", eapPassword);
        prefs.putString("eapIdentity", eapIdentity);
        prefs.putString("eapAnon",     eapAnon);
      }
      prefs.putString("sensorId",     sensorId);
      prefs.putString("userId",       userId);
      prefs.putUInt  ("checksum",     checksum);
      prefs.end();

      // Lance la connexion Wi-Fi côté loop/RTOS (ton code existant)
      setPhase(ProvPhase::CONNECTING);
      provisioningSetStatus("{\"status\":\"connecting\"}");

      // >>> Ton code Wi-Fi doit appeler ces callbacks au bon moment :
      // breezly_on_wifi_ok();
      // breezly_on_wifi_auth_failed();    // en cas d’échec auth
      // breezly_on_wifi_assoc_timeout();  // si pas de réponse AP
      // breezly_on_inet_ok();             // quand ping/HTTP OK
      // breezly_on_mqtt_hello_ok();       // quand publish MQTT “hello” (retained) OK
      // breezly_on_registered();          // quand le cloud répond “sensor attached”
      // breezly_on_connected_final();     // tout est prêt
      needToConnectWiFi = true;
      continue;
    }

    // Si on arrive ici : op non reconnue
    provisioningSetStatus("{\"status\":\"unknown_op\"}");
  }
}


// ------------------- callbacks -------------------
class ServerCb : public NimBLEServerCallbacks {
public:
  void onConnect(NimBLEServer* s, NimBLEConnInfo& ci) override {
    s_gattConnected = true;
    s_connHandle = ci.getConnHandle();
    setAppFg(true);
    setPhase(ProvPhase::STARTING);
    wd_enable(true);
    provisioningSetStatus("{\"status\":\"ble_connected\"}");
    Serial.printf("[WD] onConnect: gatt=1, conn=%u\n", (unsigned)s_connHandle);
  }

  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& ci, int reason) override {
    (void)ci;
    Serial.printf("[WD] onDisconnect: reason=%d\n", reason);
    s_gattConnected = false;
    s_connHandle = BLE_HS_CONN_HANDLE_NONE;
    setPhase(ProvPhase::IDLE);
    setAppFg(true);
    provisioningSetStatus("{\"status\":\"idle\"}");
    g_acc = "";
    wd_enable(false);
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

    const bool done = g_acc.endsWith("}");
    xSemaphoreGive(sAccMutex);

    if (!done) return;

    if (!sCredTask){
      xTaskCreatePinnedToCore(credWorker, "BLE_CRED", 8192, nullptr, 1, &sCredTask, 0);
    }
    xTaskNotifyGive(sCredTask);
  }
};

// ------------------- watchdog task -------------------
static void sessionWatchdog(void*){
  Serial.println("[WD] task started");
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));
    if (!s_wdEnabled)     continue;
    if (!s_gattConnected) continue;

    const uint32_t now  = millis();
    const uint32_t idle = now - s_lastActivityMs;

    static uint32_t lastLog = 0;
    if (now - lastLog > 5000) {
      Serial.printf("[WD] enabled=1, gatt=1, idle=%lu / phaseTO=%lu ms (phase=%u, fg=%u)\n",
        (unsigned long)idle, (unsigned long)computePhaseTimeoutMs(),
        (unsigned)s_phase, (unsigned)s_appForeground);
      lastLog = now;
    }

    const uint32_t to = computePhaseTimeoutMs();

    // Pré-warning ~10s avant coupure (réarmé par wd_kick)
    if (!s_warned && idle + 10000UL > to && idle < to) {
      provisioningSetStatus("{\"status\":\"idle_warning\",\"in\":\"10s\"}");
      s_warned = true;
      continue;
    }

    if (idle > to) {
      Serial.println("[WD] TIMEOUT -> notify + DISCONNECT");
      provisioningSetStatus("{\"status\":\"timeout\"}");
      g_acc = "";
      if (pServer && s_connHandle != BLE_HS_CONN_HANDLE_NONE) {
        pServer->disconnect(s_connHandle);
      } else {
        restartBLEAdvertising();
      }
      s_warned = false;
      wd_kick(); // évite rafale
    }
  }
}

// ------------------- API -------------------
void setupBLE(bool startAdvertising){
  wd_kick();
  Serial.printf("[WD] init: baseTO=%lu ms\n", (unsigned long)PROV_IDLE_TIMEOUT_MS);

  if (s_bleInitDone) {
    if (startAdvertising && !s_advertising) {
      configureAdvertising();
      NimBLEDevice::startAdvertising();
      s_advertising = NimBLEDevice::getAdvertising()->isAdvertising();
      Serial.printf("[BLE] re-entry startAdvertising -> %d\n", (int)s_advertising);
    }
    return;
  }

  // Nom BLE persistant
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

  // Init NimBLE
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

  pService->start();
  delay(50);

  configureAdvertising();
  delay(20);

  if (startAdvertising) {
    NimBLEDevice::startAdvertising();
    s_advertising = NimBLEDevice::getAdvertising()->isAdvertising();
    Serial.printf("[BLE] advertising=%d\n", (int)s_advertising);
  } else {
    Serial.println("[BLE] Ready (no advertising)");
  }

  s_bleInitDone = true;
  bleInited = true;

  if (!sWatchdogTask) {
    xTaskCreatePinnedToCore(sessionWatchdog, "BLE_WD", 4096, nullptr, 1, &sWatchdogTask, 0);
  }
}

void restartBLEAdvertising(){
  if (!s_bleInitDone) return;
  NimBLEAdvertising* a = NimBLEDevice::getAdvertising();
  if (!a) return;

  configureAdvertising();
  a->stop();
  delay(80);
  NimBLEDevice::startAdvertising();
  s_advertising = a->isAdvertising();
  Serial.printf("[BLE] restart -> advertising=%d\n", (int)s_advertising);
}
