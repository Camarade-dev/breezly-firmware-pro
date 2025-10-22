#include <Arduino.h>
#include <time.h>
#include <sys/time.h>
#include <WiFiClientSecure.h>
#include <stdlib.h>   // getenv, setenv, unsetenv
extern "C" {
  #include "esp_sntp.h"
}
#include "sntp_utils.h"

// --- état local SNTP ---
static bool sntpStarted = false;

// === time utils ===
static inline time_t timegm_esp(struct tm* tm) {
  char* oldtz = getenv("TZ");
  if (oldtz) oldtz = strdup(oldtz);
  setenv("TZ", "UTC", 1);
  tzset();
  time_t t = mktime(tm);    // interprété comme UTC car TZ=UTC
  if (oldtz) { setenv("TZ", oldtz, 1); free(oldtz); }
  else { unsetenv("TZ"); }
  tzset();
  return t;
}

bool timeIsSaneHard() {
  const time_t MIN_VALID = 1704067200;   // 2024-01-01 00:00:00 UTC
  return time(nullptr) >= MIN_VALID;
}

// === SNTP API ===
void stopSNTP() {
  if (sntpStarted) {
    esp_sntp_stop();
    sntpStarted = false;
  }
}

void startSNTPAfterConnected() {
  if (sntpStarted) return;
  sntpStarted = true;

  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  // Tu peux en ajouter d'autres si tu veux :
  // esp_sntp_setservername(1, "time.google.com");
  // esp_sntp_setservername(2, "fr.pool.ntp.org");
  esp_sntp_init();

  // Attente active (max ~20 s)
  for (int i = 0; i < 80; ++i) {  // 80 * 250ms ≈ 20s
    if (timeIsSaneHard()) { stopSNTP(); break; }
    delay(250);
  }
}

// === Fallback HTTP Date (si UDP/123 bloqué) ===
static bool syncTimeFromHttpDate(int timeoutMs = 6000) {
  WiFiClientSecure c;
  c.setInsecure();                           // juste pour lire l'entête Date
  c.setTimeout(timeoutMs/1000 + 1);

  if (!c.connect("google.com", 443)) return false;

  c.print("HEAD / HTTP/1.1\r\nHost: google.com\r\nConnection: close\r\n\r\n");
  unsigned long t0 = millis();
  while (c.connected() && (millis()-t0) < (unsigned long)timeoutMs) {
    String line = c.readStringUntil('\n');
    if (line.length() == 0) break;
    line.trim();
    if (line.startsWith("Date:")) {
      // Ex: "Date: Tue, 21 Oct 2025 10:17:00 GMT"
      struct tm tm{}; char wk[8], monStr[4];
      int d,y,H,M,S;
      if (sscanf(line.c_str(), "Date: %3s, %d %3s %d %d:%d:%d GMT",
                 wk, &d, monStr, &y, &H, &M, &S) == 7) {
        static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
        const char* p = strstr(months, monStr);
        int mon = p ? int((p - months)/3) : 0;
        tm.tm_year = y - 1900; tm.tm_mon = mon; tm.tm_mday = d;
        tm.tm_hour = H; tm.tm_min = M; tm.tm_sec = S;
        time_t t = timegm_esp(&tm);
        if (t > 0) {
          struct timeval tv{ .tv_sec=(long)t, .tv_usec=0 };
          settimeofday(&tv, nullptr);
          return true;
        }
      }
    }
  }
  return false;
}

// === Gate TLS clock ===
void ensureTlsClockReady(uint32_t maxWaitMs) {
  unsigned long t0 = millis();
  while (!timeIsSaneHard() && (millis() - t0) < maxWaitMs) {
    delay(250);
  }
  if (!timeIsSaneHard()) {
    (void)syncTimeFromHttpDate();   // essaie un fallback
  }
}
