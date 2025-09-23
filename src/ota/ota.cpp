#include "ota.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include "mbedtls/sha256.h"
#include "mbedtls/pk.h"
#include "mbedtls/base64.h"
#include "esp_heap_caps.h"
#include <ArduinoJson.h>
#include "../core/globals.h"
#include "../app_config.h"
#include "../led/led_status.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include "esp_ota_ops.h"
#include "esp_system.h"

static volatile bool g_otaInProgress = false;
bool otaIsInProgress(){ return g_otaInProgress; }

static int cmpSemver(const String& a, const String& b) {
  auto parse = [](const String&s,int idx){int v=0; int i=idx; while(i<(int)s.length() && isDigit(s[i])) { v = v*10 + (s[i]-'0'); i++; } return v; };
  int ia=0, ib=0;
  for (int k=0;k<3;k++){
    int va=parse(a, ia); int vb=parse(b, ib);
    if (va!=vb) return (va<vb)?-1:1;
    while (ia<(int)a.length() && (a[ia]=='.' || !isDigit(a[ia]))) ia++;
    while (ib<(int)b.length() && (b[ib]=='.' || !isDigit(b[ib]))) ib++;
    if (ia>=a.length() && ib>=b.length()) break;
  }
  return 0;
}
static uint8_t hexchar_to_val(char c){
  if (c>='0'&&c<='9') return c-'0';
  if (c>='a'&&c<='f') return 10+(c-'a');
  if (c>='A'&&c<='F') return 10+(c-'A');
  return 0xFF;
}
static bool hexEq(const uint8_t* digest, const String& hex){
  if (hex.length()!=64) return false;
  for (int i=0;i<32;i++){
    uint8_t hi = hexchar_to_val(hex[2*i]);
    uint8_t lo = hexchar_to_val(hex[2*i+1]);
    if (hi>0x0F || lo>0x0F) return false;
    if (digest[i] != (uint8_t)((hi<<4)|lo)) return false;
  }
  return true;
}

// ====== 1) VALIDATION / ROLLBACK AU BOOT ======
void otaOnBootValidate(){
  Preferences p; p.begin("ota", false);
  bool pending = p.getBool("pending", false);
  uint32_t fail = p.getUShort("fail", 0);
  p.end();

  // Si l’app précédente a flashé puis reboot → marquer valide
  if (pending){
    // Si on crashe en boucle avant d’arriver ici, le compteur "fail" grimpera
    Preferences q; q.begin("ota", false);
    uint32_t lastBoots = q.getUShort("bootcnt", 0) + 1;
    q.putUShort("bootcnt", lastBoots);
    q.putUShort("fail", fail + 1);
    q.end();

    // Après N boots sans marquage → rollback auto
    const uint32_t MAX_FAILS = 3;
    if (fail + 1 >= MAX_FAILS){
#if ESP_IDF_VERSION_MAJOR >= 4
      Serial.println("[OTA] Too many failed boots → rollback");
      esp_ota_mark_app_invalid_rollback_and_reboot();
#else
      Serial.println("[OTA] Too many failed boots (IDF<4) → reboot factory");
      const esp_partition_t* factory = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
      if (factory){ esp_ota_set_boot_partition(factory); }
      ESP.restart();
#endif
    }
  }

  // Si on atteint cette étape, c’est que l’app tourne bien → valider & reset compteurs
#if ESP_IDF_VERSION_MAJOR >= 4
  esp_ota_mark_app_valid_cancel_rollback();
#endif
  Preferences r; r.begin("ota", false);
  r.putBool("pending", false);
  r.putUShort("fail", 0);
  r.putUShort("bootcnt", 0);
  r.end();
  Serial.println("[OTA] App marked VALID");
}

// ====== 2) VERIF SIGNATURE MANIFEST (ECDSA P-256) ======
// Public key (PEM) — mets la clé **publique** correspondante à ta clé privée côté serveur
// Exemple: prime256v1 (secp256r1)
static const char* OTA_PUBKEY_PEM =
"-----BEGIN PUBLIC KEY-----\n"
"MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEZqf6uoHvTKIEuo5gmUe7KzHw38v+\n"
"5cUJ2Syyv7MWCPThb/dlMBudHsuHSkPo6IIHEv0QRILRecA1bT/Y4oucgA==\n"
"-----END PUBLIC KEY-----\n";

// On signe coté serveur le **blob canonique** (voir publish.js)
// Ici on vérifie sig(Base64) de ce blob via SHA-256/ECDSA P-256
static bool verifyManifestSignature(const String& canonical, const String& sigB64){
  mbedtls_pk_context pk; mbedtls_pk_init(&pk);
  if (mbedtls_pk_parse_public_key(&pk, (const unsigned char*)OTA_PUBKEY_PEM, strlen(OTA_PUBKEY_PEM)+1) != 0){
    mbedtls_pk_free(&pk); return false;
  }
  // decode base64
  size_t olen=0; unsigned char sig[128];
  if (mbedtls_base64_decode(sig, sizeof(sig), &olen, (const unsigned char*)sigB64.c_str(), sigB64.length())!=0){
    mbedtls_pk_free(&pk); return false;
  }
  // hash
  unsigned char hash[32];
  mbedtls_sha256_context ctx; mbedtls_sha256_init(&ctx); mbedtls_sha256_starts_ret(&ctx, 0);
  mbedtls_sha256_update_ret(&ctx, (const unsigned char*)canonical.c_str(), canonical.length());
  mbedtls_sha256_finish_ret(&ctx, hash); mbedtls_sha256_free(&ctx);

  int ok = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, olen);
  mbedtls_pk_free(&pk);
  return ok==0;
}

// ====== 3) DOWNLOAD & FLASH (ta logique inchangée + petits gardes-fous) ======
static bool httpDownloadToUpdate(const String& binUrl, uint32_t expectedSize, const String& expectedSha256Hex, WiFiClientSecure& wcs) {
  mbedtls_sha256_context ctx; mbedtls_sha256_init(&ctx); mbedtls_sha256_starts_ret(&ctx,0);
  uint8_t digest[32];

  HTTPClient http; http.setConnectTimeout(8000); http.setTimeout(8000);

  Serial.printf("[OTA] GET bin: %s\n", binUrl.c_str());
  if (!http.begin(wcs, binUrl)) { Serial.println("[OTA] http.begin FAIL"); return false; }
  http.addHeader("Accept-Encoding", "identity");
  int code=http.GET(); 
  if (code!=HTTP_CODE_OK){ Serial.printf("[OTA] code=%d\n", code); http.end(); return false; }

  int contentLen=http.getSize(); // -1 si chunked
  if (expectedSize>0 && contentLen>0 && (uint32_t)contentLen!=expectedSize) {
    Serial.printf("[OTA] Content-Length mismatch: hdr=%d, manifest=%u\n", contentLen, (unsigned)expectedSize);
    http.end(); return false;
  }

  // Check partition size (sécurité)
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* next = esp_ota_get_next_update_partition(NULL);
  if (!next){ Serial.println("[OTA] No OTA partition found"); http.end(); return false; }
  if (expectedSize>0 && expectedSize > next->size){
    Serial.printf("[OTA] Firmware too large for partition (%u > %u)\n", (unsigned)expectedSize, (unsigned)next->size);
    http.end(); return false;
  }

  if (!Update.begin((expectedSize>0)? expectedSize : (contentLen>0? (size_t)contentLen : UPDATE_SIZE_UNKNOWN))){
    Serial.printf("[OTA] Update.begin err=%u\n", Update.getError()); http.end(); return false;
  }

  const size_t CH=2048; uint8_t*buf=(uint8_t*)heap_caps_malloc(CH,MALLOC_CAP_8BIT);
  if(!buf){ Serial.println("[OTA] malloc FAIL"); http.end(); Update.end(); return false; }

  size_t written=0; WiFiClient*stream=http.getStreamPtr();
  unsigned long lastLog=0;

  g_otaInProgress = true;
  otaInProgress = true; // conserve ta globale
  updateLedState(LED_UPDATING);

  while(http.connected()){
    size_t avail=stream->available();
    if (avail){
      int n = avail>CH ? CH : avail;
      int c = stream->readBytes(buf, n);
      if (c>0){
        mbedtls_sha256_update_ret(&ctx, buf, c);
        size_t w = Update.write(buf, c);
        if (w!=(size_t)c){ 
          Serial.printf("[OTA] write err=%u\n", Update.getError()); 
          free(buf); http.end(); Update.abort(); mbedtls_sha256_free(&ctx);
          g_otaInProgress=false; otaInProgress=false; updateLedState(LED_BAD);
          return false; 
        }
        written+=c;
      }
      esp_task_wdt_reset(); vTaskDelay(1);
    } else {
      if (contentLen>0 && written >= (size_t)contentLen) break;
      vTaskDelay(1);
    }

    if (millis()-lastLog>2000){
      lastLog=millis(); 
      if (contentLen>0){
        int pct = (int)((written*100ULL)/contentLen);
        Serial.printf("[OTA] %d%% (%u/%u)\n", pct, (unsigned)written, (unsigned)contentLen);
      } else {
        Serial.printf("[OTA] written=%u (chunked)\n", (unsigned)written);
      }
    }
  }
  free(buf); http.end();

  if (!Update.end()){ 
    Serial.printf("[OTA] end err=%u\n", Update.getError()); 
    mbedtls_sha256_free(&ctx); g_otaInProgress=false; otaInProgress=false; updateLedState(LED_BAD);
    return false; 
  }

  // SHA256 finale
  mbedtls_sha256_finish_ret(&ctx, digest); mbedtls_sha256_free(&ctx);

  if (expectedSize>0 && written!=expectedSize){
    Serial.printf("[OTA] size mismatch: written=%u, expected=%u\n", (unsigned)written, (unsigned)expectedSize);
    g_otaInProgress=false; otaInProgress=false; updateLedState(LED_BAD);
    return false;
  }
  if (!hexEq(digest, expectedSha256Hex)){
    Serial.println("[OTA] SHA256 mismatch — ABORT");
    g_otaInProgress=false; otaInProgress=false; updateLedState(LED_BAD);
    return false;
  }

  if (!Update.isFinished()){ Serial.println("[OTA] not finished"); g_otaInProgress=false; otaInProgress=false; updateLedState(LED_BAD); return false; }

  Preferences p; 
  p.begin("ota", false); 
  p.putBool("pending", true); // sera validé au prochain boot dans otaOnBootValidate()
  p.putUShort("fail", 0);
  p.end();

  Serial.printf("[OTA] OK flashed %u bytes. Reboot...\n", (unsigned)written);
  delay(250); ESP.restart(); 
  return true;
}

// ====== 4) CHECK CLOUD OTA (manifest signé + staged rollout + backoff) ======
static unsigned long g_lastCheckMs = 0;
static bool g_forceCheck = false;
void triggerOtaCheckNow(){ g_forceCheck = true; }

void checkAndPerformCloudOTA(){
  if (WiFi.status()!=WL_CONNECTED){ Serial.println("[OTA] WiFi KO"); return; }
  if (g_otaInProgress || otaInProgress){ return; }

  // Backoff simple: max 1 check / 30 min (sauf trigger)
  const unsigned long PERIOD = 30UL*60UL*1000UL;
  if (!g_forceCheck && (millis() - g_lastCheckMs) < PERIOD) return;
  g_lastCheckMs = millis();
  g_forceCheck = false;

  // TLS : au choix —
  // 1) Pin CA (recommandé) : setCACert(LE_ISRG_ROOT_X1_PEM)
  // 2) Pin SPKI/empreinte (si tu veux du pinning strict)
  WiFiClientSecure wcs; 
  wcs.setInsecure(); // => remplace par setCACert(...) en prod si possible

  HTTPClient http;
  String manifestUrl = String(FW_MANIFEST_URL);
  // si tu publies latest.json → déjà ok ; sinon remplace manifest.json -> latest.json
  manifestUrl.replace("manifest.json", "latest.json");

  String url = manifestUrl + "?t=" + String(millis());
  Serial.printf("[OTA] GET manifest: %s\n", url.c_str());
  if (!http.begin(wcs, url)) { Serial.println("[OTA] http.begin FAIL"); return; }
  http.addHeader("Accept-Encoding", "identity");
  http.setReuse(false); http.useHTTP10(true);

  int code = http.GET(); 
  if (code!=HTTP_CODE_OK){ Serial.printf("[OTA] manifest code=%d\n", code); http.end(); return; }
  String body = http.getString(); http.end();

  Serial.println("[OTA] Manifest (first 300):");
  Serial.println(body.substring(0,300));

  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err){ Serial.printf("[OTA] JSON parse error: %s\n", err.c_str()); return; }

  const char* product = doc["product"] | "";
  const char* model   = doc["model"]   | "";
  const char* channel = doc["channel"] | "";
  const char* ver     = doc["version"] | "";
  const char* bin     = doc["url"]     | "";
  uint32_t expectedSize = doc["size"] | 0;
  const char* shaHex  = doc["sha256"]  | "";
  const char* sigB64  = doc["sig"]     | "";     // <— NOUVEAU
  int rollout         = doc["rollout"] | 100;    // 0..100
  bool force          = doc["force"]   | false;
  const char* minver  = doc["min_version"] | "";
  JsonArray blocked   = doc["blocked_versions"].isNull()? JsonArray() : doc["blocked_versions"].as<JsonArray>();

  if (!ver[0] || !bin[0]){ Serial.println("[OTA] champs manquants"); return; }

  // Canonical blob pour signature — doit matcher publish.js
  String canonical = String("{") +
    "\"product\":\""+String(product)+"\","+
    "\"model\":\""+String(model)+"\","+
    "\"channel\":\""+String(channel)+"\","+
    "\"version\":\""+String(ver)+"\","+
    "\"url\":\""+String(bin)+"\","+
    "\"size\":"+String(expectedSize)+","+
    "\"sha256\":\""+String(shaHex)+"\""+
  "}";

  if (!sigB64[0] || !verifyManifestSignature(canonical, String(sigB64))){
    Serial.println("[OTA] Signature INVALID → refuse update");
    return;
  }

  // Gate: product/model/channel
  if (String(model) != String("wroom32e")){ Serial.println("[OTA] model mismatch"); return; }

  // Blocked versions?
  for (JsonVariant v : blocked){
    if (String(v.as<const char*>()) == String(CURRENT_FIRMWARE_VERSION)){
      Serial.println("[OTA] current version explicitly blocked → must update");
      force = true;
    }
  }

  // min_version ?
  if (minver[0] && cmpSemver(String(CURRENT_FIRMWARE_VERSION), String(minver)) < 0){
    Serial.println("[OTA] below min_version → force update");
    force = true;
  }

  // Staged rollout : hachage stable sur sensorId → 0..99
  auto hash32 = [](const String&s){
    uint32_t h=2166136261u; for (size_t i=0;i<s.length();++i){ h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h;
  };
  uint8_t bucket = (uint8_t)(hash32(sensorId) % 100);
  if (!force && bucket >= rollout){
    Serial.printf("[OTA] rollout=%d%% → device bucket=%u → skip for now\n", rollout, bucket);
    return;
  }

  // Déjà à jour ?
  if (!force && cmpSemver(String(CURRENT_FIRMWARE_VERSION), String(ver)) >= 0){
    Serial.println("[OTA] déjà à jour"); return;
  }

  // TLS options: si tu veux pin CA :
  // wcs.setCACert(LE_ISRG_ROOT_X1_PEM); // <— colle le PEM de l’AC racine
  // (tu peux garder setInsecure le temps des tests)

  (void)product; (void)channel; // non utilisés plus loin
  httpDownloadToUpdate(String(bin), expectedSize, String(shaHex), wcs);
}
