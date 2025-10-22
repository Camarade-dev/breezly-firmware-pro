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
#include "../net/sntp_utils.h"
#include "../core/globals.h"
#include "../app_config.h"
#include "../led/led_status.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "ca_bundle.h"
#ifndef OTA_DEBUG
#define OTA_DEBUG 2   // 0 = off, 1 = normal, 2 = très verbeux
#endif

#define OTA_LOG(fmt, ...)        do{ if(OTA_DEBUG>=1) Serial.printf(fmt "\n", ##__VA_ARGS__); }while(0)
#define OTA_VLOG(fmt, ...)       do{ if(OTA_DEBUG>=2) Serial.printf(fmt "\n", ##__VA_ARGS__); }while(0)
#define OTA_HEAP(tag)            do{ if(OTA_DEBUG>=2) Serial.printf("[OTA] %s heap=%u\n", tag, (unsigned)esp_get_free_heap_size()); }while(0)

static volatile bool g_otaInProgress = false;
bool otaIsInProgress(){ return g_otaInProgress; }

// --- utils log partition ---
static void logPartitions(){
  const esp_partition_t* run = esp_ota_get_running_partition();
  const esp_partition_t* next= esp_ota_get_next_update_partition(NULL);
  if (run)  OTA_VLOG("[OTA] running  part: label=%s addr=0x%06x size=%u", run->label,  run->address,  run->size);
  if (next) OTA_VLOG("[OTA] next OTA part: label=%s addr=0x%06x size=%u", next->label, next->address, next->size);
}

// --- semver identique à ton code ---
static int cmpSemver(const String& a, const String& b) {
  auto nextNum = [](const String& s, int& idx) -> long {
    // Skip non-digits
    while (idx < (int)s.length() && !isDigit(s[idx])) idx++;
    // Read digits
    long v = 0; bool hasDigit = false;
    while (idx < (int)s.length() && isDigit(s[idx])) { v = v*10 + (s[idx]-'0'); idx++; hasDigit = true; }
    return hasDigit ? v : 0; // segment absent => 0
  };

  int ia = 0, ib = 0;
  for (int k = 0; k < 3; ++k) {
    long va = nextNum(a, ia);
    long vb = nextNum(b, ib);
    if (va != vb) return (va < vb) ? -1 : 1;
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
  uint32_t bootcnt = p.getUShort("bootcnt", 0);
  p.end();

  OTA_LOG("[OTA] BootValidate: pending=%d fail=%u bootcnt=%u", (int)pending, (unsigned)fail, (unsigned)bootcnt);
  logPartitions();
  OTA_HEAP("boot");

  if (pending){
    Preferences q; q.begin("ota", false);
    uint32_t lastBoots = q.getUShort("bootcnt", 0) + 1;
    q.putUShort("bootcnt", lastBoots);
    q.putUShort("fail", fail + 1);
    q.end();

    const uint32_t MAX_FAILS = 3;
    if (fail + 1 >= MAX_FAILS){
#if ESP_IDF_VERSION_MAJOR >= 4
      OTA_LOG("[OTA] Too many failed boots → rollback");
      esp_ota_mark_app_invalid_rollback_and_reboot();
#else
      OTA_LOG("[OTA] Too many failed boots (IDF<4) → reboot factory");
      const esp_partition_t* factory = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
      if (factory){ esp_ota_set_boot_partition(factory); }
      ESP.restart();
#endif
    }
  }

#if ESP_IDF_VERSION_MAJOR >= 4
  esp_ota_mark_app_valid_cancel_rollback();
#endif
  Preferences r; r.begin("ota", false);
  r.putBool("pending", false);
  r.putUShort("fail", 0);
  r.putUShort("bootcnt", 0);
  r.end();
  OTA_LOG("[OTA] App marked VALID");
}

// ====== 2) VERIF SIGNATURE MANIFEST (ECDSA P-256) ======
static const char* OTA_PUBKEY_PEM =
"-----BEGIN PUBLIC KEY-----\n"
"MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEZqf6uoHvTKIEuo5gmUe7KzHw38v+\n"
"5cUJ2Syyv7MWCPThb/dlMBudHsuHSkPo6IIHEv0QRILRecA1bT/Y4oucgA==\n"
"-----END PUBLIC KEY-----\n";

static bool verifyManifestSignature(const String& canonical, const String& sigB64){
  OTA_VLOG("[OTA] verify sig: canonical_len=%u sigB64_len=%u", (unsigned)canonical.length(), (unsigned)sigB64.length());
  mbedtls_pk_context pk; mbedtls_pk_init(&pk);
  if (mbedtls_pk_parse_public_key(&pk, (const unsigned char*)OTA_PUBKEY_PEM, strlen(OTA_PUBKEY_PEM)+1) != 0){
    OTA_LOG("[OTA] parse public key FAIL");
    mbedtls_pk_free(&pk); return false;
  }
  size_t olen=0; unsigned char sig[128];
  if (mbedtls_base64_decode(sig, sizeof(sig), &olen, (const unsigned char*)sigB64.c_str(), sigB64.length())!=0){
    OTA_LOG("[OTA] base64 decode FAIL");
    mbedtls_pk_free(&pk); return false;
  }
  OTA_VLOG("[OTA] sig decoded len=%u", (unsigned)olen);
  unsigned char hash[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx); 
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, (const unsigned char*)canonical.c_str(), canonical.length());
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);

  int ok = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, olen);
  mbedtls_pk_free(&pk);
  OTA_LOG("[OTA] manifest signature %s", ok==0 ? "OK" : "INVALID");
  return ok==0;
}
// ====== DOWNLOAD & FLASH : TLS manuel simple + retries ======
static bool httpDownloadToUpdate(const String& binUrl,
                                 uint32_t expectedSize,
                                 const String& expectedSha256Hex,
                                 WiFiClientSecure& wcs) {
  OTA_LOG("[OTA] Download start (TLS manual)");
  OTA_HEAP("before tls");
  logPartitions();

  if (!binUrl.startsWith("https://")) { OTA_LOG("[OTA] URL must be https"); return false; }
  String hostPath = binUrl.substring(strlen("https://"));
  int slash = hostPath.indexOf('/');
  if (slash < 0) { OTA_LOG("[OTA] Bad URL"); return false; }
  String host = hostPath.substring(0, slash);
  String path = hostPath.substring(slash);

  g_otaInProgress = true; otaInProgress = true; updateLedState(LED_UPDATING);

  wcs.setCACert(CA_BUNDLE_PEM);
  wcs.setTimeout(30000);

  bool tlsOK = false;
  for (int i=1;i<=3 && !tlsOK;i++){
    OTA_LOG("[OTA] TLS connect to %s:443 (try %d/3)", host.c_str(), i);
    if (wcs.connect(host.c_str(), 443)) tlsOK = true;
    else delay(300);
  }
  if (!tlsOK){
    OTA_LOG("[OTA] TLS connect FAIL");
    g_otaInProgress=false; otaInProgress=false; updateLedState(LED_BAD);
    return false;
  }

  String req;
  req.reserve(256);
  req += "GET " + path + " HTTP/1.1\r\n";
  req += "Host: " + host + "\r\n";
  req += "User-Agent: esp32-ota\r\n";
  req += "Accept: */*\r\n";
  req += "Accept-Encoding: identity\r\n";
  req += "Connection: close\r\n\r\n";

  if (wcs.print(req) != (int)req.length()) {
    OTA_LOG("[OTA] write request FAIL");
    wcs.stop(); g_otaInProgress=false; otaInProgress=false; updateLedState(LED_BAD);
    return false;
  }

  OTA_LOG("[OTA] waiting headers...");
  unsigned long t0 = millis();
  String statusLine = wcs.readStringUntil('\n'); statusLine.trim();
  OTA_LOG("[OTA] status: %s", statusLine.c_str());
  if (!statusLine.startsWith("HTTP/1.1 200") && !statusLine.startsWith("HTTP/1.0 200")) {
    OTA_LOG("[OTA] HTTP not 200");
    wcs.stop(); g_otaInProgress=false; otaInProgress=false; updateLedState(LED_BAD);
    return false;
  }

  int contentLen = -1;
  bool chunked = false;
  while (true) {
    String h = wcs.readStringUntil('\n'); h.trim();
    if (h.length()==0) break;
    if (h.startsWith("Content-Length:") || h.startsWith("content-length:"))
      contentLen = h.substring(h.indexOf(':')+1).toInt();
    else if (h.startsWith("Transfer-Encoding:") && h.indexOf("chunked")>0)
      chunked = true;
  }
  OTA_LOG("[OTA] headers: len=%d chunked=%d", contentLen, (int)chunked);
  if (chunked) { OTA_LOG("[OTA] chunked not supported"); wcs.stop(); g_otaInProgress=false; otaInProgress=false; updateLedState(LED_BAD); return false; }
  if (expectedSize>0 && contentLen>0 && (uint32_t)contentLen!=expectedSize) {
    OTA_LOG("[OTA] length mismatch hdr=%d expected=%u", contentLen, (unsigned)expectedSize);
    wcs.stop(); g_otaInProgress=false; otaInProgress=false; updateLedState(LED_BAD); return false;
  }

  const esp_partition_t* next = esp_ota_get_next_update_partition(NULL);
  if (!next){ OTA_LOG("[OTA] No OTA partition"); wcs.stop(); g_otaInProgress=false; otaInProgress=false; updateLedState(LED_BAD); return false; }
  size_t beginSize = (expectedSize>0)? expectedSize : (contentLen>0? (size_t)contentLen : UPDATE_SIZE_UNKNOWN);
  if (beginSize!=UPDATE_SIZE_UNKNOWN && beginSize > next->size) {
    OTA_LOG("[OTA] image too large (%u>%u)", (unsigned)beginSize, (unsigned)next->size);
    wcs.stop(); g_otaInProgress=false; otaInProgress=false; updateLedState(LED_BAD); return false;
  }
  if (!Update.begin(beginSize)) {
    OTA_LOG("[OTA] Update.begin err=%u", Update.getError());
    wcs.stop(); g_otaInProgress=false; otaInProgress=false; updateLedState(LED_BAD); return false;
  }

  const size_t CH = 2048;
  uint8_t* buf = (uint8_t*)heap_caps_malloc(CH, MALLOC_CAP_8BIT);
  if (!buf) { OTA_LOG("[OTA] malloc FAIL"); wcs.stop(); Update.end(); g_otaInProgress=false; otaInProgress=false; updateLedState(LED_BAD); return false; }

  mbedtls_sha256_context sha; mbedtls_sha256_init(&sha); mbedtls_sha256_starts(&sha, 0);
  size_t written = 0;
  unsigned long lastLog = millis();
  unsigned long lastProgress = millis();
  const unsigned long NO_PROGRESS_ABORT_MS = 30000;

  while (wcs.connected() || wcs.available()) {
    int n = wcs.read(buf, CH);
    if (n > 0) {
      if (written==0) OTA_LOG("[OTA] first chunk %d bytes", n);
      lastProgress = millis();
      mbedtls_sha256_update(&sha, buf, n);

      size_t w = Update.write(buf, n);
      if (w != (size_t)n) {
        OTA_LOG("[OTA] write err=%u (wrote=%u/got=%d)", Update.getError(), (unsigned)w, n);
        free(buf); wcs.stop(); Update.abort(); mbedtls_sha256_free(&sha);
        g_otaInProgress=false; otaInProgress=false; updateLedState(LED_BAD); return false;
      }
      written += n;

      if (contentLen>0 && (millis()-lastLog)>2000) {
        lastLog = millis();
        int pct = (int)((written*100ULL)/contentLen);
        OTA_LOG("[OTA] %d%% (%u/%u)", pct, (unsigned)written, (unsigned)contentLen);
        OTA_HEAP("stream");
      }
    } else {
      if (!wcs.connected() && wcs.available()==0) break;
      if (millis()-lastProgress > NO_PROGRESS_ABORT_MS) {
        OTA_LOG("[OTA] no progress for %lu ms → abort", (unsigned long)(millis()-lastProgress));
        free(buf); wcs.stop(); Update.abort(); mbedtls_sha256_free(&sha);
        g_otaInProgress=false; otaInProgress=false; updateLedState(LED_BAD); return false;
      }
      delay(5);
    }
    esp_task_wdt_reset();
  }

  free(buf);
  wcs.stop();

  if (!Update.end()) {
    OTA_LOG("[OTA] end err=%u", Update.getError());
    mbedtls_sha256_free(&sha); g_otaInProgress=false; otaInProgress=false; updateLedState(LED_BAD); return false;
  }

  uint8_t digest[32]; mbedtls_sha256_finish(&sha, digest); mbedtls_sha256_free(&sha);
  if (expectedSize>0 && written!=expectedSize) {
    OTA_LOG("[OTA] size mismatch: written=%u expected=%u", (unsigned)written, (unsigned)expectedSize);
    g_otaInProgress=false; otaInProgress=false; updateLedState(LED_BAD); return false;
  }
  if (!hexEq(digest, expectedSha256Hex)) {
    OTA_LOG("[OTA] SHA256 mismatch");
    g_otaInProgress=false; otaInProgress=false; updateLedState(LED_BAD); return false;
  }
  if (!Update.isFinished()) {
    OTA_LOG("[OTA] not finished");
    g_otaInProgress=false; otaInProgress=false; updateLedState(LED_BAD); return false;
  }

  unsigned long dt = millis()-t0;
  float kb = written/1024.0f;
  float kbps = dt? (1000.0f*kb/dt) : 0.0f;
  OTA_LOG("[OTA] OK flashed %u bytes in %lums (%.1f KB/s)", (unsigned)written, dt, kbps);

  Preferences p; p.begin("ota", false); p.putBool("pending", true); p.putUShort("fail", 0); p.end();

  OTA_LOG("[OTA] Reboot...");
  delay(250);
  ESP.restart();
  return true;
}


// ====== 4) CHECK CLOUD OTA ======
static unsigned long g_lastCheckMs = 0;
static bool g_forceCheck = false;
void triggerOtaCheckNow(){ g_forceCheck = true; }

void checkAndPerformCloudOTA(){
  OTA_VLOG("[OTA] check enter: wifi=%d inProg=%d force=%d", (WiFi.status()==WL_CONNECTED), (int)g_otaInProgress, (int)g_forceCheck);
  if (WiFi.status()!=WL_CONNECTED){ OTA_LOG("[OTA] WiFi KO"); return; }
    ensureTlsClockReady(20000);
  if (!timeIsSaneHard()) { OTA_LOG("[OTA] clock not sane → skip"); return; }
  if (g_otaInProgress || otaInProgress){ OTA_VLOG("[OTA] already in progress"); return; }

  const unsigned long PERIOD = 30UL*60UL*1000UL;
  //if (!g_forceCheck && (millis() - g_lastCheckMs) < PERIOD){
  //  OTA_VLOG("[OTA] throttled: next in %lus", (unsigned)((PERIOD - (millis()-g_lastCheckMs))/1000UL));
  //  return;
  //}
  g_lastCheckMs = millis();
  bool forced = g_forceCheck;
  g_forceCheck = false;

  WiFiClientSecure wcs; 
  wcs.setCACert(CA_BUNDLE_PEM);

  HTTPClient http;

  String manifestUrl = String(FW_MANIFEST_URL);
  manifestUrl.replace("manifest.json", "latest.json");

  String url = manifestUrl + "?t=" + String(millis());
  OTA_LOG("[OTA] GET manifest: %s (forced=%d)", url.c_str(), (int)forced);
  if (!http.begin(wcs, url)) { OTA_LOG("[OTA] http.begin FAIL"); return; }
  http.addHeader("Accept-Encoding", "identity");
  http.setReuse(false); http.useHTTP10(true);
  http.setTimeout(5000);      // 5 s max par read
  http.setReuse(false);
  http.useHTTP10(true);

  int code = http.GET(); 
  OTA_LOG("[OTA] manifest code=%d", code);
  
  if (code!=HTTP_CODE_OK){ http.end(); return; }
  String body = http.getString();    // peut bloquer jusqu'au timeout
  esp_task_wdt_reset();
  http.end();

  OTA_VLOG("[OTA] Manifest head:\n%s", body.substring(0,300).c_str());

  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err){ OTA_LOG("[OTA] JSON parse error: %s", err.c_str()); return; }

  const char* product = doc["product"] | "";
  const char* model   = doc["model"]   | "";
  const char* channel = doc["channel"] | "";
  const char* ver     = doc["version"] | "";
  const char* bin     = doc["url"]     | "";
  uint32_t expectedSize = doc["size"] | 0;
  const char* shaHex  = doc["sha256"]  | "";
  const char* sigB64  = doc["sig"]     | "";
  int rollout         = doc["rollout"] | 100;
  bool force          = doc["force"]   | false;
  const char* minver  = doc["min_version"] | "";
  JsonArray blocked   = doc["blocked_versions"].isNull()? JsonArray() : doc["blocked_versions"].as<JsonArray>();

  OTA_LOG("[OTA] manifest: ver=%s size=%u rollout=%d force=%d min=%s", ver, (unsigned)expectedSize, rollout, (int)force, minver);
  OTA_VLOG("[OTA] model=%s channel=%s product=%s", model, channel, product);

  if (!ver[0] || !bin[0]){ OTA_LOG("[OTA] champs manquants"); return; }

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
    OTA_LOG("[OTA] Signature INVALID → refuse update");
    return;
  }

  if (String(model) != String("wroom32e")){ OTA_LOG("[OTA] model mismatch (%s!=wroom32e)", model); return; }

  for (JsonVariant v : blocked){
    if (String(v.as<const char*>()) == String(CURRENT_FIRMWARE_VERSION)){
      OTA_LOG("[OTA] version courante bloquée → force update");
      force = true;
    }
  }

  if (minver[0] && cmpSemver(String(CURRENT_FIRMWARE_VERSION), String(minver)) < 0){
    OTA_LOG("[OTA] below min_version (%s < %s) → force update", CURRENT_FIRMWARE_VERSION, minver);
    force = true;
  }

  auto hash32 = [](const String&s){
    uint32_t h=2166136261u; for (size_t i=0;i<s.length();++i){ h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h;
  };
  uint8_t bucket = (uint8_t)(hash32(sensorId) % 100);
  OTA_LOG("[OTA] rollout=%d%% bucket=%u sensorId=%s", rollout, bucket, sensorId.c_str());
  if (!force && bucket >= rollout){
    OTA_LOG("[OTA] skip by rollout");
    return;
  }

  int cmp = cmpSemver(String(CURRENT_FIRMWARE_VERSION), String(ver));
  OTA_LOG("[OTA] version cmp: current=%s target=%s -> %d", CURRENT_FIRMWARE_VERSION, ver, cmp);
  if (!force && cmp >= 0){
    OTA_LOG("[OTA] déjà à jour (skip)");
    return;
  }

  OTA_LOG("[OTA] UPDATE → %s", bin);
  OTA_HEAP("before flash");
  logPartitions();
  (void)product; (void)channel;
  WiFiClientSecure binClient;
  binClient.setCACert(CA_BUNDLE_PEM);
  binClient.setTimeout(5000);         // ajoute ceci
  httpDownloadToUpdate(String(bin), expectedSize, String(shaHex), binClient);
}
