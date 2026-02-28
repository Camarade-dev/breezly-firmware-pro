// src/sensors/sensors.cpp
#include "sensors.h"
#include <Wire.h>
#include <cstring>
#include "../app_config.h"
#include "../core/log.h"
#include "calibration.h"
#include <math.h>
#ifndef PMS_LOG_BURST
#define PMS_LOG_BURST 1   // 1 = affiche les rafales PMS dans le Serial
#endif
// ==== extern définis dans core/globals.h (NE PAS changer les types) ====
extern HardwareSerial& PMS;   // <- référence, pas un objet
extern PmsData gPms;
extern SemaphoreHandle_t gPmsMutex;
extern uint16_t be16(const uint8_t *b); // déjà déclaré dans globals.h

// =======================================================================
// ===================== Réglages UX (fusion + lissage) ==================
// =======================================================================

// Affine "usine" (+ petit plancher pour éviter 0 moches)
static constexpr float kPM1_Af  = 1.000f;
static constexpr float kPM1_Bf  = 0.0f;
static constexpr float kPM25_Af = 2.000f;
static constexpr float kPM25_Bf = 1.5f;
static constexpr float kPM10_Af = 2.100f;
static constexpr float kPM10_Bf = 1.5f;

// Activ. fusion counts
static constexpr uint16_t kCNT_MIN_FOR_FLOOR = 50;

// Gains faibles (estimations par counts)
static constexpr float kPM25_CNT_K = 0.005f;   // µg/m3 / count
static constexpr float kPM10_CNT_K = 0.008f;   // µg/m3 / count
static constexpr float kPM1_CNT_K  = 0.0035f;  // µg/m3 / count

// Planchers doux si counts>seuil
static constexpr float kPM_MIN_FLOOR_25 = 1.0f;
static constexpr float kPM_MIN_FLOOR_10 = 1.0f;

// EMA adaptatif (constantes de temps)
static constexpr float kTAU_FAST   = 2.0f;  // pics
static constexpr float kTAU_MID    = 4.0f;  // petits évènements
static constexpr float kTAU_NORMAL = 8.0f;  // régime normal
static constexpr float kTAU_CLEAN  = 10.0f; // air très propre

// =======================================================================
// =========================== Etat filtre courant =======================
// =======================================================================

struct PmsFiltState {
  uint32_t lastMs = 0;
  float pm1 = NAN, pm25 = NAN, pm10 = NAN;
} s_pmsFilt;

// =======================================================================
// ============================== Utilitaires ============================
// =======================================================================

static inline float pmAffine(float x, float A, float B){ return A * x + B; }

static inline float emaAlpha(uint32_t dt_ms, float tau_sec){
  float dt = dt_ms / 1000.0f;
  return 1.0f - expf(-dt / fmaxf(tau_sec, 1e-3f));
}

static inline float emaStepAdaptive(float prev, float x, uint32_t dt_ms){
  if (!isfinite(prev)) return x;
  float dx = (dt_ms>0) ? fabsf(x - prev) : 0.0f;
  float tau;
  if (dx > 3.0f)      tau = kTAU_FAST;
  else if (dx > 1.0f) tau = kTAU_MID;
  else if (x < 1.0f)  tau = kTAU_CLEAN;
  else                tau = kTAU_NORMAL;
  float a = emaAlpha(dt_ms, tau);
  return prev + a * (x - prev);
}

// Estimations douces par counts
static inline float pm25FromCounts(uint16_t gt03, uint16_t gt10, uint16_t gt25){
  return kPM25_CNT_K * (0.6f*gt03 + 0.3f*gt10 + 0.1f*gt25);
}
static inline float pm10FromCounts(uint16_t gt10, uint16_t gt25, uint16_t gt50){
  return kPM10_CNT_K * (0.7f*gt10 + 0.2f*gt25 + 0.1f*gt50);
}
static inline float pm1FromCounts(uint16_t gt03, uint16_t gt05){
  return kPM1_CNT_K * (0.7f*gt03 + 0.3f*gt05);
}

// Poids de fusion β en fonction de la densité de counts + “escaliers”
static inline float betaFromCountsShape(float pm_affine, float countsDensity, float betaMin, float betaMax){
  float beta = 0.0f;
  if (pm_affine < 2.0f && countsDensity > kCNT_MIN_FOR_FLOOR){
    float t = (countsDensity - 50.0f) / (400.0f - 50.0f);
    if (t<0) t=0; if (t>1) t=1;
    beta = betaMin + (betaMax - betaMin) * t;
  }
  // boost léger si valeur "pile un entier" (effet escaliers)
  if (fabsf(pm_affine - roundf(pm_affine)) < 0.05f) {
    beta = fmaxf(beta, (betaMin + betaMax) * 0.5f);
  }
  return beta;
}

static inline float fusePm25(float pm25_affine, uint16_t gt03, uint16_t gt10, uint16_t gt25){
  float est = pm25FromCounts(gt03, gt10, gt25);
  float density = (float)gt03 + gt10 + gt25;
  float beta = betaFromCountsShape(pm25_affine, density, 0.15f, 0.60f);
  float fused = (1.0f - beta)*pm25_affine + beta*est;
  if (pm25_affine <= 0.1f && density > kCNT_MIN_FOR_FLOOR)
    fused = fmaxf(fused, kPM_MIN_FLOOR_25);
  return fused;
}

static inline float fusePm10(float pm10_affine, uint16_t gt10, uint16_t gt25, uint16_t gt50){
  float est = pm10FromCounts(gt10, gt25, gt50);
  float density = (float)gt10 + gt25 + gt50;
  float beta = betaFromCountsShape(pm10_affine, density, 0.10f, 0.50f);
  float fused = (1.0f - beta)*pm10_affine + beta*est;
  if (pm10_affine <= 0.1f && density > kCNT_MIN_FOR_FLOOR)
    fused = fmaxf(fused, kPM_MIN_FLOOR_10);
  return fused;
}

static inline float fusePm1_conservative(float pm1_affine, uint16_t gt03, uint16_t gt05){
  // β plus faible pour ne pas “gonfler” artificiellement PM1 en air propre
  float est = pm1FromCounts(gt03, gt05);
  float density = (float)gt03 + gt05;
  float beta = 0.0f;
  if (pm1_affine < 1.0f && density > kCNT_MIN_FOR_FLOOR){
    float t = (density - 50.0f) / (400.0f - 50.0f);
    if (t<0) t=0; if (t>1) t=1;
    beta = 0.10f + 0.30f*t; // 0.10 → 0.40
  }
  return (1.0f - beta)*pm1_affine + beta*est;
}

// =======================================================================
// ============================ Post-traitement ==========================
// =======================================================================

void pmsPostProcess(const PmsData& in, float& pm1, float& pm25, float& pm10){
  // 1) affine
  float a1  = pmAffine((float)in.pm1_atm,  kPM1_Af,  kPM1_Bf);
  float a25 = pmAffine((float)in.pm25_atm, kPM25_Af, kPM25_Bf);
  float a10 = pmAffine((float)in.pm10_atm, kPM10_Af, kPM10_Bf);

  // 2) fusion counts
  float f25 = fusePm25(a25, in.gt03, in.gt10, in.gt25);
  float f10 = fusePm10(a10, in.gt10, in.gt25, in.gt50);
  float f1  = fusePm1_conservative(a1, in.gt03, in.gt05); // option “conservatrice”

  // clamp
  if (f1  < 0.f)  f1  = 0.f;
  if (f25 < 0.f)  f25 = 0.f;
  if (f10 < 0.f)  f10 = 0.f;

  // 3) EMA adaptatif
  uint32_t now = millis();
  uint32_t dt  = (s_pmsFilt.lastMs==0) ? 0 : (now - s_pmsFilt.lastMs);
  s_pmsFilt.pm1  = emaStepAdaptive(s_pmsFilt.pm1,  f1,  dt);
  s_pmsFilt.pm25 = emaStepAdaptive(s_pmsFilt.pm25, f25, dt);
  s_pmsFilt.pm10 = emaStepAdaptive(s_pmsFilt.pm10, f10, dt);
  s_pmsFilt.lastMs = now;

  pm1  = s_pmsFilt.pm1;
  pm25 = s_pmsFilt.pm25;
  pm10 = s_pmsFilt.pm10;
}

// =======================================================================
// ========================== Gestion du PMS (IO) ========================
// =======================================================================

static bool pmsStarted = false;
static int s_pmsSetPin = -1;
static bool pmsAlwaysOn = PMS_ALWAYS_ON;
static volatile bool s_pmsAwake = false;

void pmsInitPins(int setPin){
  s_pmsSetPin = setPin;
  pinMode(setPin, OUTPUT);
  if (pmsAlwaysOn) { digitalWrite(setPin, HIGH); s_pmsAwake = true; }
  else             { digitalWrite(setPin, LOW);  s_pmsAwake = false; }
}

void pmsWake(){  if (s_pmsSetPin>=0) digitalWrite(s_pmsSetPin, HIGH); s_pmsAwake=true; }
void pmsSleep(){ if (s_pmsSetPin>=0) digitalWrite(s_pmsSetPin, LOW);  s_pmsAwake=false; }

// Lecture d’une trame PMS (utilise be16() fourni par globals.h)
static bool readPmsFrame(HardwareSerial &ser, PmsData &out) {
  while (ser.available() >= 32) {
    if ((uint8_t)ser.peek()!=0x42){ ser.read(); continue; }
    if (ser.available() < 2) return false;
    uint8_t b0=ser.read(), b1=ser.read(); if (b0!=0x42 || b1!=0x4D) continue;

    uint8_t payload[30]; if (ser.readBytes(payload,30)!=30) return false;
    uint32_t sum = b0 + b1; for (int i=0;i<28;i++) sum += payload[i];
    uint16_t chk = be16(&payload[28]); if ((sum & 0xFFFF)!=chk) return false;

    uint16_t fl = be16(&payload[0]); if (fl!=28) return false;

    out.pm1_cf1  = be16(&payload[2]);  out.pm25_cf1 = be16(&payload[4]);  out.pm10_cf1 = be16(&payload[6]);
    out.pm1_atm  = be16(&payload[8]);  out.pm25_atm = be16(&payload[10]); out.pm10_atm = be16(&payload[12]);
    out.gt03     = be16(&payload[14]); out.gt05     = be16(&payload[16]); out.gt10     = be16(&payload[18]);
    out.gt25     = be16(&payload[20]); out.gt50     = be16(&payload[22]); out.gt100    = be16(&payload[24]);
    #if PMS_LOG_BURST
        Serial.printf("PMS: CF1 PM1=%u PM2.5=%u PM10=%u | ATM PM1=%u PM2.5=%u PM10=%u | CNT 0.3=%u 0.5=%u 1.0=%u 2.5=%u 5.0=%u 10=%u\n",
                      out.pm1_cf1, out.pm25_cf1, out.pm10_cf1,
                      out.pm1_atm, out.pm25_atm, out.pm10_atm,
                      out.gt03, out.gt05, out.gt10, out.gt25, out.gt50, out.gt100);
    #endif
    out.valid=true; out.lastMs=millis(); return true;
  }
  return false;
}

static void pmsTask(void *){
  PMS.begin(9600, SERIAL_8N1, 16, 17);
  uint32_t t0=millis(); while (millis()-t0<5000) vTaskDelay(100/portTICK_PERIOD_MS);
  PmsData tmp; static uint32_t seq=0;
  for(;;){
    if (s_pmsAwake && readPmsFrame(PMS, tmp)){
      tmp.seq = ++seq;
      if (gPmsMutex && xSemaphoreTake(gPmsMutex, 5/portTICK_PERIOD_MS)==pdTRUE){
        gPms = tmp; xSemaphoreGive(gPmsMutex);
      }
    }
    vTaskDelay(s_pmsAwake ? 50/portTICK_PERIOD_MS : 250/portTICK_PERIOD_MS);
  }
}

void pmsTaskStart(int rx, int tx){
  (void)rx; (void)tx; // câblés en dur
  if (pmsStarted) return;
  xTaskCreatePinnedToCore(pmsTask, "PMS", 4096, 0, 1, 0, 0);
  pmsStarted = true;
}

// Spot blocking : wake → warmup → lit dernière trame récente → sleep
bool pmsSampleBlocking(uint32_t warmupMs, PmsData& out){
  pmsWake();
  uint32_t t0 = millis();
  while ((millis() - t0) < warmupMs){
    vTaskDelay(100/portTICK_PERIOD_MS);
  }

  bool have = false;
  if (gPmsMutex && xSemaphoreTake(gPmsMutex, 100/portTICK_PERIOD_MS) == pdTRUE){
    if (gPms.valid && (millis() - gPms.lastMs) < 5000) { out = gPms; have = true; }
    xSemaphoreGive(gPmsMutex);
  }
  pmsSleep();
  return have;
}

// =======================================================================
// ======================= AHT21 / ENS160 + I2C robustesse ================
// =======================================================================

static uint8_t s_i2cConsecutiveFailures = 0;

static void i2cBusReset(){
  Wire.end();
  delay(50);
  Wire.begin();
  Wire.setClock(100000);
#if defined(ARDUINO_ARCH_ESP32)
  Wire.setTimeOut((uint16_t)I2C_BUS_TIMEOUT_MS);
#endif
  (void)aht.begin();
  (void)ens160.begin();
  ens160.setMode(ENS160_OPMODE_STD);
  LOGI("I2C", "bus reset after %d failures, sensors re-init", (int)I2C_BUS_RESET_AFTER_FAILURES);
}

bool sensorsInit(){
  Wire.begin();
  delay(100);
  Wire.setClock(100000);
#if defined(ARDUINO_ARCH_ESP32)
  Wire.setTimeOut((uint16_t)I2C_BUS_TIMEOUT_MS);
#endif
  if (!aht.begin())  Serial.println("AHT21 initialization failed!");
  else               Serial.println("AHT21 initialisé avec succès.");

  if (!ens160.begin()) Serial.println("Échec ENS160 !");
  else { Serial.println("ENS160 initialisé avec succès."); ens160.setMode(ENS160_OPMODE_STD); }

  s_i2cConsecutiveFailures = 0;
  if (!gPmsMutex) gPmsMutex = xSemaphoreCreateMutex();
  return true;
}

static inline float satVapor_hPa(float T){
  return 6.112f * expf(17.62f * T / (243.12f + T));
}
static inline float rhTempCompensate(float RH_raw, float T_raw, float T_corr){
  if (!isfinite(RH_raw) || !isfinite(T_raw) || !isfinite(T_corr)) return NAN;
  if (fabsf(T_corr - T_raw) < 0.001f) return RH_raw;
  const float es_raw  = satVapor_hPa(T_raw);
  const float es_corr = satVapor_hPa(T_corr);
  if (es_corr <= 0.0f) return NAN;
  float RH = RH_raw * (es_raw / es_corr);
  if (RH < 0.f)   RH = 0.f;
  if (RH > 100.f) RH = 100.f;
  return RH;
}

bool safeSensorRead(float& tempC, float& humidity){
  sensors_event_t eventHum, eventTemp;
  aht.getEvent(&eventHum, &eventTemp);
  if (isnan(eventTemp.temperature) || isnan(eventHum.relative_humidity)){
    s_i2cConsecutiveFailures++;
    if (s_i2cConsecutiveFailures >= I2C_BUS_RESET_AFTER_FAILURES){
      i2cBusReset();
      s_i2cConsecutiveFailures = 0;
    }
    return false;
  }
  s_i2cConsecutiveFailures = 0;

  const float T_raw = eventTemp.temperature;
  const float RH_raw = eventHum.relative_humidity;

  const float T_corr = calApplyTemp(T_raw);
  float RH_tc = rhTempCompensate(RH_raw, T_raw, T_corr);

  float RH_corr = /* calApplyHum( */ RH_tc /* ) */;
  if (!isfinite(RH_corr)) RH_corr = RH_tc;
  if (RH_corr < 0.f)   RH_corr = 0.f;
  if (RH_corr > 100.f) RH_corr = 100.f;

  tempC   = T_corr;
  humidity= RH_corr;
  return true;
}

void sensorsReadEns160(int& aqi, int& tvoc, int& eco2, float tempC, float humidity){
  if (ens160.available()){
    ens160.set_envdata(tempC, humidity);
    ens160.measure(true);
    aqi  = ens160.getAQI();
    tvoc = ens160.getTVOC();
    eco2 = ens160.geteCO2();
  } else { aqi=tvoc=eco2=0; }
}

bool sensorSanityCheck(int aqi, int tvoc, int eco2, char* failOut, size_t failOutSize){
  bool okAqi  = (aqi >= SANITY_AQI_MIN && aqi <= SANITY_AQI_MAX);
  bool okTvoc = (tvoc >= 0 && (unsigned long)tvoc <= SANITY_TVOC_MAX_PPB);
  bool okEco2 = (eco2 >= SANITY_ECO2_MIN_PPM && eco2 <= SANITY_ECO2_MAX_PPM);
  if (failOut && failOutSize > 0) failOut[0] = '\0';
  if (okAqi && okTvoc && okEco2) return true;
  if (!failOut || failOutSize == 0) return false;
  char* p = failOut;
  size_t remain = failOutSize;
  auto append = [&](const char* s) {
    size_t len = std::strlen(s);
    if (len + 2 > remain) return;
    if (p != failOut) { *p++ = ','; remain--; }
    std::memcpy(p, s, len + 1);
    p += len;
    remain -= len;
  };
  if (!okAqi)  append("aqi");
  if (!okTvoc) append("tvoc");
  if (!okEco2) append("eco2");
  return false;
}
