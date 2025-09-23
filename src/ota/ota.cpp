#include "ota.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include "mbedtls/sha256.h"
#include "esp_heap_caps.h"
#include <ArduinoJson.h>
#include "../core/globals.h"
#include "../app_config.h"
#include "../led/led_status.h"
#include <Arduino.h>
#include <esp_task_wdt.h>
static int cmpSemver(const String& a, const String& b) {
  auto parse = [](const String&s,int idx){int v=0; int i=idx; while(i<(int)s.length() && isDigit(s[i])) { v = v*10 + (s[i]-'0'); i++; } return v; };
  int ia=0, ib=0;
  for (int k=0;k<3;k++){
    int va=parse(a, ia); int vb=parse(b, ib);
    if (va!=vb) return (va<vb)?-1:1;
    // skip non-digits / dots
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


bool httpDownloadToUpdate(const String& binUrl, uint32_t expectedSize, const String& expectedSha256Hex) {
  mbedtls_sha256_context ctx; mbedtls_sha256_init(&ctx); mbedtls_sha256_starts_ret(&ctx,0);
  uint8_t digest[32];

  WiFiClientSecure wcs; wcs.setInsecure();
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

  // Démarre l'Update avec la taille attendue si dispo
  if (!Update.begin((expectedSize>0)? expectedSize : (contentLen>0? (size_t)contentLen : UPDATE_SIZE_UNKNOWN))){
    Serial.printf("[OTA] Update.begin err=%u\n", Update.getError()); http.end(); return false;
  }

  const size_t CH=2048; uint8_t*buf=(uint8_t*)heap_caps_malloc(CH,MALLOC_CAP_8BIT);
  if(!buf){ Serial.println("[OTA] malloc FAIL"); http.end(); Update.end(); return false; }

  size_t written=0; WiFiClient*stream=http.getStreamPtr();
  unsigned long lastLog=0;

  otaInProgress = true; // protège le système
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
          free(buf); http.end(); Update.abort(); mbedtls_sha256_free(&ctx); otaInProgress=false; updateLedState(LED_BAD);
          return false; 
        }
        written+=c;
      }
      // feed watchdog / yield
      esp_task_wdt_reset(); vTaskDelay(1);
    } else {
      // fin?
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
    mbedtls_sha256_free(&ctx); otaInProgress=false; updateLedState(LED_BAD);
    return false; 
  }

  // SHA256 finale
  mbedtls_sha256_finish_ret(&ctx, digest); mbedtls_sha256_free(&ctx);

  if (expectedSize>0 && written!=expectedSize){
    Serial.printf("[OTA] size mismatch: written=%u, expected=%u\n", (unsigned)written, (unsigned)expectedSize);
    otaInProgress=false; updateLedState(LED_BAD);
    return false;
  }
  if (!hexEq(digest, expectedSha256Hex)){
    Serial.println("[OTA] SHA256 mismatch — ABORT");
    otaInProgress=false; updateLedState(LED_BAD);
    return false;
  }

  if (!Update.isFinished()){ Serial.println("[OTA] not finished"); otaInProgress=false; updateLedState(LED_BAD); return false; }

  Preferences p; 
  p.begin("ota", false); 
  p.putBool("pending", true); 
  p.end();

  Serial.printf("[OTA] OK flashed %u bytes. Reboot...\n", (unsigned)written);
  delay(250); ESP.restart(); 
  return true; // (n’atteint pas)
}

void checkAndPerformCloudOTA(){
  if (WiFi.status()!=WL_CONNECTED){ Serial.println("[OTA] WiFi KO"); return; }
  if (otaInProgress){ return; }

  WiFiClientSecure wcs; wcs.setInsecure(); HTTPClient http;

  // utilise latest.json si dispo (sinon FW_MANIFEST_URL actuel)
  String manifestUrl = String(FW_MANIFEST_URL); // ex: .../manifest.json
  // Optionnel: si tu publies latest.json, remplace par .../latest.json
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

  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err){ Serial.printf("[OTA] JSON parse error: %s\n", err.c_str()); return; }

  const char* ver = doc["version"] | "";
  const char* bin = doc["url"]     | "";
  uint32_t expectedSize = doc["size"] | 0;
  const char* shaHex = doc["sha256"] | "";

  if (!ver[0] || !bin[0]){ Serial.println("[OTA] champs manquants"); return; }

  if (cmpSemver(String(CURRENT_FIRMWARE_VERSION), String(ver)) >= 0){
    Serial.println("[OTA] déjà à jour"); return;
  }

  httpDownloadToUpdate(String(bin), expectedSize, String(shaHex));
}
