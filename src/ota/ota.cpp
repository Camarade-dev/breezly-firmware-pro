#include "ota.h"
#include "../net/mqtt_bus.h"
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
#include "esp_err.h"

extern "C" {
  #include "esp_wifi.h"
}

#ifndef OTA_DEBUG
#define OTA_DEBUG 2   // 0 = off, 1 = normal, 2 = verbose
#endif

#define OTA_LOG(fmt, ...)  do{ if(OTA_DEBUG>=1) Serial.printf(fmt "\n", ##__VA_ARGS__); }while(0)
#define OTA_VLOG(fmt, ...) do{ if(OTA_DEBUG>=2) Serial.printf(fmt "\n", ##__VA_ARGS__); }while(0)
#define OTA_HEAP(tag)      do{ if(OTA_DEBUG>=2) Serial.printf("[OTA] %s heap=%u\n", tag, (unsigned)esp_get_free_heap_size()); }while(0)
struct BoolFlagGuard {
  volatile bool* p;
  explicit BoolFlagGuard(volatile bool* ptr) : p(ptr) { if (p) *p = true; }
  ~BoolFlagGuard() { if (p) *p = false; }
  BoolFlagGuard(const BoolFlagGuard&) = delete;
  BoolFlagGuard& operator=(const BoolFlagGuard&) = delete;
};
// ===================== Utils =====================
static volatile bool g_otaInProgress = false;
bool otaIsInProgress(){ return g_otaInProgress; }
extern volatile bool g_otaBootWindowDone;
struct OtaBootWindowGuard {
  volatile bool* flag;
  explicit OtaBootWindowGuard(volatile bool* p) : flag(p) {}
  ~OtaBootWindowGuard() { if (flag) *flag = true; }
};
void otaSetInProgress(bool v){ g_otaInProgress = v; }
static bool httpGetToString(const String& url, String& out,
                            uint32_t connectMs = 8000,
                            uint32_t readMs    = 12000);
static void logPartitions(){
  /*const esp_partition_t* run  = esp_ota_get_running_partition();
  const esp_partition_t* next = esp_ota_get_next_update_partition(NULL);
  if (run)  OTA_VLOG(
      "[OTA] running  part: label=%s addr=0x%06x size=%u encrypted=%d",
      run->label,  run->address,  run->size, run->encrypted
  );
  if (next) OTA_VLOG(
      "[OTA] next OTA part: label=%s addr=0x%06x size=%u encrypted=%d",
      next->label, next->address, next->size, next->encrypted
  );*/
}
static bool fetchWithRetry(const String& u, String& out, int tries=4) {
  for (int i=1; i<=tries; ++i) {
    if (httpGetToString(u, out, 15000, 15000)) return true;
    OTA_LOG("[OTA] fetch fail try %d/%d", i, tries);
    // backoff + jitter pour laisser le LB/campus respirer
    uint32_t d = 300 * i + (esp_random() % 200);
    delay(d);
  }
  return false;
}
static int cmpSemver(const String& a, const String& b) {
  auto nextNum = [](const String& s, int& idx) -> long {
    while (idx < (int)s.length() && !isDigit(s[idx])) idx++;
    long v = 0; bool hasDigit=false;
    while (idx < (int)s.length() && isDigit(s[idx])) { v = v*10 + (s[idx]-'0'); idx++; hasDigit=true; }
    return hasDigit ? v : 0;
  };
  int ia=0, ib=0;
  for (int k=0;k<3;k++){ long va=nextNum(a,ia), vb=nextNum(b,ib); if (va!=vb) return (va<vb)?-1:1; }
  return 0;
}

static uint8_t hex_nibble(char c){
  if (c>='0'&&c<='9') return c-'0';
  if (c>='a'&&c<='f') return 10+(c-'a');
  if (c>='A'&&c<='F') return 10+(c-'A');
  return 0xFF;
}
static bool hexEq(const uint8_t* digest, const String& hex){
  if (hex.length()!=64) return false;
  for (int i=0;i<32;i++){
    uint8_t hi=hex_nibble(hex[2*i]), lo=hex_nibble(hex[2*i+1]);
    if (hi>0x0F||lo>0x0F) return false;
    if (digest[i] != (uint8_t)((hi<<4)|lo)) return false;
  }
  return true;
}
static bool downloadAndFlashWithHTTPClient(const String& binUrl,
                                           uint32_t expectedSize,
                                           const String& expectedSha256Hex);

// GET → String via HTTPClient (TLS 1.2+, no chunked in memory)
// Remplace ta httpGetToString existante par celle-ci
static bool httpGetToString(const String& url, String& out,
                            uint32_t connectMs,
                            uint32_t readMs) {
  WiFiClientSecure wcs;
  wcs.setCACert(CA_BUNDLE_PEM);

#if defined(ARDUINO_ARCH_ESP32) && !defined(ARDUINO_ESP32S2_DEV)
  wcs.setHandshakeTimeout(connectMs);
#endif
  wcs.setTimeout(readMs);

  HTTPClient http;
  if (!http.begin(wcs, url)) {
    OTA_LOG("[OTA] http.begin FAIL: %s", url.c_str());
    return false;
  }

  http.addHeader("Accept", "*/*");
  http.addHeader("Accept-Encoding", "identity");
  http.addHeader("Connection", "close");
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setConnectTimeout(connectMs);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    OTA_LOG("[OTA] GET %s -> code=%d (%s)",
            url.c_str(), code, http.errorToString(code).c_str());
    http.end();
    return false;
  }

  out = http.getString();
  http.end();

  OTA_LOG("[OTA] manifest OK, %u bytes", (unsigned)out.length());
  return out.length() > 0;
}





// ===================== Boot validation / rollback =====================
void otaOnBootValidate(){
  Preferences p; p.begin("ota", false);
  bool     pending = p.getBool("pending", false);
  uint16_t fail    = p.getUShort("fail", 0);
  uint16_t bootcnt = p.getUShort("bootcnt", 0);
  p.end();

  OTA_LOG("[OTA] BootValidate: pending=%d fail=%u bootcnt=%u", (int)pending, (unsigned)fail, (unsigned)bootcnt);
  logPartitions();
  OTA_HEAP("boot");

  const uint16_t MAX_FAILS = 3;

  if (pending){
    if (fail >= MAX_FAILS){
      // We are the partition that was rolled back TO (the good one). Clear pending and mark valid; do not rollback again.
      Preferences r; r.begin("ota", false);
      r.putBool("pending", false);
      r.putUShort("fail", 0);
      r.putUShort("bootcnt", 0);
      r.end();
#if ESP_IDF_VERSION_MAJOR >= 4
      esp_ota_mark_app_valid_cancel_rollback();
#endif
      OTA_LOG("[OTA] Rollback target: clear pending, mark valid");
      return;
    }
    Preferences q; q.begin("ota", false);
    q.putUShort("bootcnt", bootcnt+1);
    q.putUShort("fail", fail+1);
    q.end();

    if ((fail+1) >= MAX_FAILS){
      Preferences rb; rb.begin("ota", false);
      rb.putString("rolled_back_ver", CURRENT_FIRMWARE_VERSION);
      rb.end();
      OTA_LOG("[OTA] Too many failed boots → rollback (record rolled_back_ver=%s)", CURRENT_FIRMWARE_VERSION);
#if ESP_IDF_VERSION_MAJOR >= 4
      esp_ota_mark_app_invalid_rollback_and_reboot();
#else
      OTA_LOG("[OTA] Too many failed boots (IDF<4) → reboot factory");
      const esp_partition_t* factory = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
      if (factory){ esp_ota_set_boot_partition(factory); }
      ESP.restart();
#endif
    }
#ifdef OTA_TEST_ROLLBACK_SIMULATE_CRASH
    else {
      OTA_LOG("[OTA] TEST: simulate failed boot %u/3 → reboot", (unsigned)(fail+1));
      delay(500);
      ESP.restart();
    }
#endif
  }

#if ESP_IDF_VERSION_MAJOR >= 4
  esp_ota_mark_app_valid_cancel_rollback();
#endif
  Preferences r; r.begin("ota", false);
  r.putBool("pending", false);
  r.putUShort("fail", 0);
  r.putUShort("bootcnt", 0);
  r.putString("rolled_back_ver", "");  // clear so future OTAs to newer versions are allowed
  r.end();
  OTA_LOG("[OTA] App marked VALID");
}

// ===================== Manifest signature (ECDSA P-256) =====================
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
  unsigned char sig[128]; size_t olen=0;
  if (mbedtls_base64_decode(sig, sizeof(sig), &olen, (const unsigned char*)sigB64.c_str(), sigB64.length())!=0){
    OTA_LOG("[OTA] base64 decode FAIL");
    mbedtls_pk_free(&pk); return false;
  }
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

// ===================== Download & flash (HTTPClient only) =====================
static bool downloadAndFlashWithHTTPClientInner(const String& binUrl,
                                                uint32_t expectedSize,
                                                const String& expectedSha256Hex,
                                                bool allowGithubFallback)
{
  OTA_LOG("[OTA] Download start (HTTPClient+SNI)");
  OTA_HEAP("before http");
  logPartitions();

  const esp_partition_t* nextPart = esp_ota_get_next_update_partition(NULL);
  if (!nextPart) {
    OTA_LOG("[OTA] No OTA partition");
    return false;
  }

  g_otaInProgress = true;
  updateLedState(LED_UPDATING);

  // --- Parse URL -> scheme/host/port/path
  String scheme = "https", host, path = "/";
  uint16_t port = 443;
  {
    int p = binUrl.indexOf("://");
    int h0 = (p > 0) ? p + 3 : 0;
    if (p > 0) scheme = binUrl.substring(0, p);
    int slash = binUrl.indexOf('/', h0);
    String hostPort = (slash > 0) ? binUrl.substring(h0, slash) : binUrl.substring(h0);
    if (slash > 0) path = binUrl.substring(slash);
    int colon = hostPort.indexOf(':');
    if (colon >= 0) {
      host = hostPort.substring(0, colon);
      port = (uint16_t)hostPort.substring(colon + 1).toInt();
    } else {
      host = hostPort;
    }
  }

  bool isGithub = (host == "Camarade-dev.github.io" || host.endsWith(".github.io"));

  // DNS (log)
  IPAddress ip;
  if (WiFi.hostByName(host.c_str(), ip)) {
    OTA_LOG("[OTA] DNS %s -> %s", host.c_str(), ip.toString().c_str());
  } else {
    OTA_LOG("[OTA] DNS resolve FAIL for %s", host.c_str());
    g_otaInProgress = false;
    updateLedState(LED_BAD);
    return false;
  }

  // Jitter pour éviter les connexions back-to-back
  delay(250 + (esp_random() % 300));

  // --- TLS client (validation cert pour tous les hôtes, dont GitHub Pages)
  WiFiClientSecure tls;
  tls.setCACert(CA_BUNDLE_PEM);

#if defined(ARDUINO_ARCH_ESP32) && !defined(ARDUINO_ESP32S2_DEV)
  tls.setHandshakeTimeout(15000);
#endif
  tls.setTimeout(45000);

  HTTPClient http;

  auto beginWithHeaders = [&]() -> bool {
    bool ok = false;

    if (isGithub) {
      // URL complète (comme pour le manifest)
      ok = http.begin(tls, binUrl);
    } else {
      bool isHttps = (scheme == "https");
      ok = http.begin(tls, host, port, path, isHttps);
    }

    if (!ok) {
      OTA_LOG("[OTA] http.begin FAIL (host=%s port=%u path=%s)",
              host.c_str(), (unsigned)port, path.c_str());
      return false;
    }

    // HTTP/1.0 → pas de chunked
    http.useHTTP10(true);

    // Headers “browser-like” safe
    http.addHeader("Accept", "*/*");
    http.addHeader("Accept-Encoding", "identity");
    http.addHeader("Connection", "close");
    http.addHeader("User-Agent",
                   "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                   "AppleWebKit/537.36 (KHTML, like Gecko) "
                   "Chrome/120.0.0.0 Safari/537.36");

    http.setReuse(false);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
#if defined(HTTPCLIENT_1_2_COMPATIBLE)
    http.setConnectTimeout(15000);
#endif
    return true;
  };

  if (!beginWithHeaders()) {
    g_otaInProgress = false;
    updateLedState(LED_BAD);
    return false;
  }

  // --- GET avec retries
  int code = -1;
  for (int attempt = 1; attempt <= 4; ++attempt) {
    code = http.GET();
    OTA_LOG("[OTA] BIN GET attempt %d -> code=%d (%s)",
            attempt, code, http.errorToString(code).c_str());

    if (code == HTTP_CODE_OK) break;

    // erreurs réseau / serveur → retry
    if (code < 0 || (code >= 500 && code < 600)) {
      http.end();
      delay(1000 * attempt + (esp_random() % 500));
      if (!beginWithHeaders()) {
        OTA_LOG("[OTA] http.begin FAIL on retry (host=%s)", host.c_str());
      }
      continue;
    }

    // 4xx → inutile d’insister
    break;
  }

  if (code != HTTP_CODE_OK) {
  OTA_LOG("[OTA] HTTPClient GET failed, code=%d (%s)",
          code, http.errorToString(code).c_str());

  OTA_LOG("[OTA] WiFi.status()=%d, RSSI=%d",
          (int)WiFi.status(), WiFi.RSSI());

  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    OTA_LOG("[OTA] AP: RSSI=%d, channel=%d",
            ap.rssi, ap.primary);
  }

  http.end();

    // 🔁 Fallback GitHub -> backend si blocage réseau (EAP etc.)
    if (isGithub && allowGithubFallback) {
      String fallback = binUrl;

      // ⚠️ même chemin relatif, juste host différent
      // GitHub: https://Camarade-dev.github.io/breezly-firmware-dist/firmware/...
      // Backend: https://breezly-backend.onrender.com/firmware/...
      fallback.replace(
        "https://Camarade-dev.github.io/breezly-firmware-dist",
        "https://breezly-backend.onrender.com"
      );

      OTA_LOG("[OTA] GitHub .bin blocked (code=%d) → trying backend: %s",
              code, fallback.c_str());

      g_otaInProgress = false;   // reset propre
      updateLedState(LED_BOOT);  // on revient à un état neutre

      // Appel récursif sans 2e fallback pour éviter boucle infinie
      return downloadAndFlashWithHTTPClientInner(
        fallback, expectedSize, expectedSha256Hex, false
      );
    }

    g_otaInProgress = false;
    updateLedState(LED_BAD);
    return false;
  }

  Client* stream = http.getStreamPtr();
  int contentLen = http.getSize();  // -1 si chunked

  // Si le serveur ne donne pas de Content-Length, on garde expectedSize
  if (!expectedSize && contentLen > 0) {
    expectedSize = (uint32_t)contentLen;
  }

  if (expectedSize && expectedSize > nextPart->size) {
    OTA_LOG("[OTA] image too large: expectedSize=%u > partSize=%u",
            (unsigned)expectedSize, (unsigned)nextPart->size);
    http.end();
    g_otaInProgress = false;
    updateLedState(LED_BAD);
    return false;
  }

  // === OTA native ESP-IDF ===
  esp_ota_handle_t otaHandle = 0;
  size_t otaSizeParam = expectedSize ? expectedSize : 0;
  esp_err_t err = esp_ota_begin(nextPart, otaSizeParam, &otaHandle);
  if (err != ESP_OK) {
    OTA_LOG("[OTA] esp_ota_begin FAILED err=%d (%s)", (int)err, esp_err_to_name(err));
    http.end();
    g_otaInProgress = false;
    updateLedState(LED_BAD);
    return false;
  }
  OTA_LOG("[OTA] esp_ota_begin OK (maxSize=%u)", (unsigned)nextPart->size);

  OTA_HEAP("before buf malloc");

  // On essaie 2048, puis 1024 si nécessaire
  size_t CH = 2048;
  uint8_t* buf = nullptr;

  while (CH >= 512 && buf == nullptr) {
    buf = (uint8_t*)heap_caps_malloc(CH, MALLOC_CAP_8BIT);
    if (!buf) {
      OTA_LOG("[OTA] heap_caps_malloc(%u) FAIL -> try smaller", (unsigned)CH);
      CH /= 2;
    }
  }

  if (!buf) {
    OTA_LOG("[OTA] malloc FAIL even at 512 bytes");
    esp_ota_abort(otaHandle);
    http.end();
    g_otaInProgress = false;
    updateLedState(LED_BAD);
    return false;
  }

  OTA_LOG("[OTA] chunk size = %u bytes", (unsigned)CH);

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);

  size_t written = 0;
  unsigned long lastLog = millis();
  unsigned long lastProgress = millis();
  static const unsigned long NO_PROGRESS_ABORT_MS = 300000;

  bool firstChunk = true;

  while (true) {
    int n = stream->readBytes(buf, CH);

    if (n < 0) {
      OTA_LOG("[OTA] stream read ERROR (%d)", n);
      free(buf);
      mbedtls_sha256_free(&sha);
      esp_ota_abort(otaHandle);
      http.end();
      g_otaInProgress = false;
      updateLedState(LED_BAD);
      return false;
    }

    if (n == 0) {
      if (!http.connected()) {
        OTA_LOG("[OTA] HTTP disconnected, end of stream");
        break;
      }
      if (millis() - lastProgress > NO_PROGRESS_ABORT_MS) {
        OTA_LOG("[OTA] no progress timeout");
        free(buf);
        mbedtls_sha256_free(&sha);
        esp_ota_abort(otaHandle);
        http.end();
        g_otaInProgress = false;
        updateLedState(LED_BAD);
        return false;
      }
      delay(5);
      esp_task_wdt_reset();
      continue;
    }

    if (firstChunk) {
      firstChunk = false;
      OTA_LOG("[OTA] first 16 bytes of image:");
      for (int i = 0; i < 16 && i < n; ++i) {
        Serial.printf("%02X ", buf[i]);
      }
      Serial.println();
    }

    lastProgress = millis();
    mbedtls_sha256_update(&sha, buf, n);

    err = esp_ota_write(otaHandle, buf, n);
    if (err != ESP_OK) {
      OTA_LOG("[OTA] esp_ota_write FAIL err=%d (%s)", (int)err, esp_err_to_name(err));
      free(buf);
      mbedtls_sha256_free(&sha);
      esp_ota_abort(otaHandle);
      http.end();
      g_otaInProgress = false;
      updateLedState(LED_BAD);
      return false;
    }

    written += n;
    if ((millis() - lastLog) > 2000){
      lastLog = millis();
      OTA_LOG("[OTA] written %u bytes...", (unsigned)written);
    }
    if ((millis() - lastLog) > 2000 && expectedSize) {
      lastLog = millis();
      int pct = (int)((written * 100ULL) / expectedSize);
      OTA_LOG("[OTA] %d%% (%u/%u)", pct, (unsigned)written, (unsigned)expectedSize);
    }

    esp_task_wdt_reset();
  }

  free(buf);

  // Fin de l'image côté OTA
  err = esp_ota_end(otaHandle);
  if (err != ESP_OK) {
    OTA_LOG("[OTA] esp_ota_end FAIL err=%d (%s)", (int)err, esp_err_to_name(err));
    mbedtls_sha256_free(&sha);
    http.end();
    g_otaInProgress = false;
    updateLedState(LED_BAD);
    return false;
  }

  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  http.end();

  if (expectedSize && written != expectedSize) {
    OTA_LOG("[OTA] size mismatch: written=%u expected=%u",
            (unsigned)written, (unsigned)expectedSize);
    g_otaInProgress = false;
    updateLedState(LED_BAD);
    return false;
  }

  if (!expectedSha256Hex.isEmpty() && !hexEq(digest, expectedSha256Hex)) {
    OTA_LOG("[OTA] SHA256 mismatch");
    g_otaInProgress = false;
    updateLedState(LED_BAD);
    return false;
  }

  // On sélectionne la nouvelle partition comme partition de boot
  err = esp_ota_set_boot_partition(nextPart);
  if (err != ESP_OK) {
    OTA_LOG("[OTA] esp_ota_set_boot_partition FAIL err=%d (%s)",
            (int)err, esp_err_to_name(err));
    g_otaInProgress = false;
    updateLedState(LED_BAD);
    return false;
  }

  OTA_LOG("[OTA] OK (%u bytes), boot partition set to %s",
          (unsigned)written, nextPart->label);

  Preferences p; p.begin("ota", false);
  p.putBool("pending", true);
  p.putUShort("fail", 0);
  String pendingVer = p.getString("pending_ver", "");
  if (pendingVer.length()) {
    p.putString("from_ver", CURRENT_FIRMWARE_VERSION);
    p.putString("success_ver", pendingVer);
    p.remove("pending_ver");
  }
  p.end();

  g_otaInProgress = false;
  delay(250);
  ESP.restart();
  return true;
}
// ===================== Download & flash (HTTPClient only) =====================
// ===================== Download & flash (HTTPClient with manual fallback) =====================
// ===================== Download & flash (HTTPClient with manual fallback) =====================
// ===================== Download & flash (HTTPClient with manual fallback, fixed types) =====================
// ===================== Download & flash (HTTPClient, no raw fallback) =====================
// ===================== Download & flash (HTTPClient with explicit SNI host) =====================
// ===================== Download & flash (HTTPClient with explicit SNI host) =====================
static bool downloadAndFlashWithHTTPClient(const String& binUrl,
                                           uint32_t expectedSize,
                                           const String& expectedSha256Hex)
{
  return downloadAndFlashWithHTTPClientInner(binUrl, expectedSize, expectedSha256Hex, true);
}





// ===================== Cloud OTA check =====================
static unsigned long g_lastCheckMs = 0;
static bool g_forceCheck = false;
void triggerOtaCheckNow(){ g_forceCheck = true; }

void checkAndPerformCloudOTA(){
  OTA_VLOG("[OTA] check enter: wifi=%d inProg=%d force=%d",
           (WiFi.status()==WL_CONNECTED), (int)g_otaInProgress, (int)g_forceCheck);
  OtaBootWindowGuard guard(&g_otaBootWindowDone);
  if (WiFi.status()!=WL_CONNECTED){ OTA_LOG("[OTA] WiFi KO"); return; }
  ensureTlsClockReady(20000);
  if (!timeIsSaneHard()) { OTA_LOG("[OTA] clock not sane → skip"); return; }
  BoolFlagGuard netGuard(&g_netBusyForOta);  // met true à l’entrée et false à la sortie

  if (g_otaInProgress){ OTA_VLOG("[OTA] already in progress"); return; }

  const unsigned long PERIOD = 30UL*60UL*1000UL;
  // if (!g_forceCheck && (millis() - g_lastCheckMs) < PERIOD) return;
  g_lastCheckMs = millis();
  bool forced = g_forceCheck;
  g_forceCheck = false;
  String url = String(FW_MANIFEST_URL);
  OTA_LOG("[OTA] GET manifest: %s (forced=%d)", url.c_str(), (int)forced);

  String body;
  Serial.printf("[OTA] Allocating manifest buffer...\n");
  body.reserve(1500);
  if (!fetchWithRetry(url, body)) {
    #ifdef APP_ENV_DEV
      String direct = "https://breezly-backendweb.onrender.com/firmware/esp32/wroom32e/dev/latest.json";
    #else
      String direct = "https://breezly-backend.onrender.com/firmware/esp32/wroom32e/prod/latest.json";
    #endif

    OTA_LOG("[OTA] primary failed → try direct: %s", direct.c_str());
    if (!fetchWithRetry(direct, body)) {
      OTA_LOG("[OTA] manifest fetch failed (both)");
      return;
    }
  }
  OTA_LOG("[OTA] manifest OK, %u bytes", (unsigned)body.length());
  OTA_VLOG("[OTA] Manifest head:\n%s", body.substring(0, 300).c_str());

  StaticJsonDocument<1024> doc;
  DeserializationError jerr = deserializeJson(doc, body);
  if (jerr) { OTA_LOG("[OTA] JSON parse error: %s", jerr.c_str()); return; }

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
  JsonArray blocked   = doc["blocked_versions"].isNull()
                        ? JsonArray() : doc["blocked_versions"].as<JsonArray>();

  OTA_LOG("[OTA] manifest: ver=%s size=%u rollout=%d force=%d min=%s",
          ver, (unsigned)expectedSize, rollout, (int)force, minver);
  OTA_VLOG("[OTA] model=%s channel=%s product=%s", model, channel, product);

  if (!ver[0] || !bin[0]) { OTA_LOG("[OTA] champs manquants"); return; }

  // Signature
  // Signature
  char canonical[512];  // 256 -> 512
  int n = snprintf(
    canonical, sizeof(canonical),
    "{\"product\":\"%s\",\"model\":\"%s\",\"channel\":\"%s\","
    "\"version\":\"%s\",\"url\":\"%s\",\"size\":%u,\"sha256\":\"%s\"}",
    product, model, channel, ver, bin, (unsigned)expectedSize, shaHex
  );
  if (n <= 0 || n >= (int)sizeof(canonical)) {
    OTA_LOG("[OTA] canonical overflow");
    return;
  }
  if (!verifyManifestSignature(String(canonical), String(sigB64))) {
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

  auto hash32 = [](const String&s){ uint32_t h=2166136261u; for (size_t i=0;i<s.length();++i){ h ^= (uint8_t)s[i]; h *= 16777619u; } return h; };
  uint8_t bucket = (uint8_t)(hash32(sensorId) % 100);
  OTA_LOG("[OTA] rollout=%d%% bucket=%u sensorId=%s", rollout, bucket, sensorId.c_str());
  if (!force && bucket >= rollout){ OTA_LOG("[OTA] skip by rollout"); return; }

    int cmp = cmpSemver(String(CURRENT_FIRMWARE_VERSION), String(ver));
  OTA_LOG("[OTA] version cmp: current=%s target=%s -> %d", CURRENT_FIRMWARE_VERSION, ver, cmp);
  if (!force && cmp >= 0){
    OTA_LOG("[OTA] déjà à jour (skip)");
    return;
  }

  Preferences pSkip; pSkip.begin("ota", true);
  String rolledBack = pSkip.getString("rolled_back_ver", "");
  pSkip.end();
  if (rolledBack.length() > 0 && String(ver) == rolledBack){
    OTA_LOG("[OTA] skip: version %s was rolled back (reject re-install)", ver);
    return;
  }

  // On n'a plus besoin du manifest : on vide au moins le contenu
  body = "";

  OTA_LOG("[OTA] UPDATE → %s", bin);
  OTA_HEAP("before flash");
  logPartitions();


  // === NEW: relancer un "setup" réseau propre entre manifest et .bin ===
  // On attend un petit délai random pour casser le pattern "manifest puis gros binaire direct"
  uint32_t cooldown = 1500 + (esp_random() % 2500);
  OTA_LOG("[OTA] cooldown before BIN download: %lu ms", (unsigned long)cooldown);
  delay(cooldown);

  // On force la fermeture de toute connexion résiduelle (au cas où un proxy
  // garde du state agressif sur des bursts très serrés). httpGetToString()
  // fait déjà http.end(), mais on isole bien la phase "manifest" de la phase "bin".
  WiFiClientSecure dummy;
  dummy.stop(); // no-op mais explicite, au cas où

  // Petit "warmup" GET ultra léger vers le même host pour ressembler davantage
  // à un client humain qui fait plusieurs petites requêtes avant un gros DL.
  // Si ça échoue (firewall), on ignore totalement.
  String binUrl = String(bin);
  int protoIdx  = binUrl.indexOf("://");
  int hostStart = (protoIdx > 0) ? protoIdx + 3 : 0;
  int pathIdx   = binUrl.indexOf('/', hostStart);

  if (hostStart > 0 && pathIdx > hostStart) {
    String warmupUrl = binUrl.substring(0, pathIdx) + "/"; // racine du host
    String warmupBody;
    OTA_LOG("[OTA] warmup GET to %s", warmupUrl.c_str());
    // 1 seule tentative, on ne check pas le retour : c'est juste pour "diluer" le pattern
    fetchWithRetry(warmupUrl, warmupBody, 1);
  }

  // Téléchargement/flash réel
  {
    String ctx;
    ctx = "{\"target_version\":\"" + String(ver) + "\"}";
    mqtt_telemetry_emit("OTA_START", ctx.c_str());
  }
  Preferences prefsOta;
  prefsOta.begin("ota", false);
  prefsOta.putString("pending_ver", ver);
  prefsOta.end();

  bool ok = downloadAndFlashWithHTTPClient(binUrl, expectedSize, String(shaHex));
  if (!ok) {
    OTA_LOG("[OTA] download/flash failed");
    char otaFailCtx[120];
    snprintf(otaFailCtx, sizeof(otaFailCtx), "{\"stage\":\"download_flash\",\"code\":-1}");
    mqtt_telemetry_emit("OTA_FAIL", otaFailCtx);
    prefsOta.begin("ota", false);
    prefsOta.remove("pending_ver");
    prefsOta.end();
  }
}

