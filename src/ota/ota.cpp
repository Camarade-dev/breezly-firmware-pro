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

bool httpDownloadToUpdate(const String& binUrl) {
  mbedtls_sha256_context ctx; mbedtls_sha256_init(&ctx); mbedtls_sha256_starts_ret(&ctx,0);
  uint8_t digest[32];

  WiFiClientSecure wcs; wcs.setInsecure();
  HTTPClient http; http.setConnectTimeout(8000); http.setTimeout(8000);

  Serial.printf("[OTA] GET bin: %s\n", binUrl.c_str());
  if (!http.begin(wcs, binUrl)) { Serial.println("[OTA] http.begin FAIL"); return false; }
  int code=http.GET(); if (code!=HTTP_CODE_OK){ Serial.printf("[OTA] code=%d\n", code); http.end(); return false; }

  int total=http.getSize(); WiFiClient*stream=http.getStreamPtr();
  if (!Update.begin(total>0?total:UPDATE_SIZE_UNKNOWN)){
    Serial.printf("[OTA] Update.begin err=%u\n", Update.getError()); http.end(); return false;
  }

  const size_t CH=2048; uint8_t*buf=(uint8_t*)heap_caps_malloc(CH,MALLOC_CAP_8BIT);
  if(!buf){ Serial.println("[OTA] malloc FAIL"); http.end(); Update.end(); return false; }

  size_t written=0; unsigned long lastLog=0;
  while(http.connected() && (total>0 || total==-1)){
    size_t avail=stream->available();
    if (avail){
      int n = avail>CH ? CH : avail;
      int c = stream->readBytes(buf, n);
      if (c>0){
        mbedtls_sha256_update_ret(&ctx, buf, c);
        size_t w = Update.write(buf, c);
        if (w!=(size_t)c){ Serial.printf("[OTA] write err=%u\n", Update.getError()); free(buf); http.end(); Update.abort(); mbedtls_sha256_free(&ctx); return false; }
        written+=c; if (total>0) total-=c;
      }
      vTaskDelay(1);
    } else vTaskDelay(1);

    if (millis()-lastLog>2000 && http.getSize()>0){
      lastLog=millis(); int sizeKnown=http.getSize(); int done=(int)written;
      int pct=(int)((done*100LL)/(sizeKnown>0?sizeKnown:1));
      Serial.printf("[OTA] %d%% (%u/%u)\n", pct, (unsigned)done, (unsigned)sizeKnown);
    }
  }
  free(buf); http.end();

  if (!Update.end()){ Serial.printf("[OTA] end err=%u\n", Update.getError()); mbedtls_sha256_free(&ctx); return false; }
  mbedtls_sha256_finish_ret(&ctx, digest); mbedtls_sha256_free(&ctx);
  if (!Update.isFinished()){ Serial.println("[OTA] not finished"); return false; }

  Serial.printf("[OTA] OK flashed %u bytes. Reboot...\n", (unsigned)written);
  delay(250); ESP.restart(); return true;
}

void checkAndPerformCloudOTA(){
  if (WiFi.status()!=WL_CONNECTED){ Serial.println("[OTA] WiFi KO"); return; }

  WiFiClientSecure wcs; wcs.setInsecure(); HTTPClient http;
  String url = String(FW_MANIFEST_URL) + "?t=" + String(millis());
  Serial.printf("[OTA] GET manifest: %s\n", url.c_str());
  if (!http.begin(wcs, url)) { Serial.println("[OTA] http.begin FAIL"); return; }

  http.addHeader("Accept-Encoding", "identity"); http.setReuse(false); http.useHTTP10(true);
  int code = http.GET(); if (code!=HTTP_CODE_OK){ Serial.printf("[OTA] manifest code=%d\n", code); http.end(); return; }
  String body = http.getString(); http.end();

  Serial.println("[OTA] Manifest (first 300):");
  Serial.println(body.substring(0,300));

  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err){ Serial.printf("[OTA] JSON parse error: %s\n", err.c_str()); return; }

  const char* ver = doc["version"] | "";
  const char* bin = doc["url"]     | "";
  if (!ver[0] || !bin[0]){ Serial.println("[OTA] champs manquants"); return; }
  if (String(ver)==CURRENT_FIRMWARE_VERSION){ Serial.println("[OTA] déjà à jour"); return; }

  httpDownloadToUpdate(String(bin));
}
