#include "sensors.h"
#include <Wire.h>
#include "../app_config.h"
#include "calibration.h"
#include <math.h>



struct PmsFiltState {
  uint32_t lastMs = 0;
  float pm1 = NAN, pm25 = NAN, pm10 = NAN;
} s_pmsFilt;

// ======= Réglages "usine" PMS (affine + anti-zéro + EMA) =======
static constexpr float kPM1_Af  = 1.000f;
static constexpr float kPM1_Bf  = 0.0f;
static constexpr float kPM25_Af = 1.000f;
static constexpr float kPM25_Bf = 1.5f;   // petit plancher usine (ex : +0.5 µg/m3)
static constexpr float kPM10_Af = 1.000f;
static constexpr float kPM10_Bf = 1.5f;

// micro-estimation à partir des counts (heuristique douce)
static constexpr uint16_t kCNT_MIN_FOR_FLOOR = 50; // on n'active qu'au-dessus
static constexpr float    kPM25_CNT_K        = 0.005f; // µg/m3 par "count" (faible)
static constexpr float    kPM10_CNT_K        = 0.008f; // µg/m3 par "count" (faible)
static constexpr float    kPM_MIN_FLOOR_25   = 1.0f;   // plancher min si counts>seuil
static constexpr float    kPM_MIN_FLOOR_10   = 1.0f;

// EMA (constante de temps)
static constexpr float kEMA_TAU_SEC = 8.0f;

// Applique l'affine usine
static inline float pmAffine(float x, float A, float B){
  return A * x + B;
}

// Anti-zéro : si lecture 0 mais counts > seuil, injecte une micro-estimation
static inline float pmAntiZeroFromCounts25(float pm25_raw, uint16_t gt03, uint16_t gt10, uint16_t gt25){
  const uint32_t sum = (uint32_t)gt03 + gt10 + gt25;
  if (pm25_raw > 0.0f || sum < kCNT_MIN_FOR_FLOOR) return pm25_raw;
  float est = kPM25_CNT_K * (0.6f*gt03 + 0.3f*gt10 + 0.1f*gt25);
  if (est < kPM_MIN_FLOOR_25) est = kPM_MIN_FLOOR_25;
  return est;
}
static inline float pmAntiZeroFromCounts10(float pm10_raw, uint16_t gt10, uint16_t gt25, uint16_t gt50){
  const uint32_t sum = (uint32_t)gt10 + gt25 + gt50;
  if (pm10_raw > 0.0f || sum < kCNT_MIN_FOR_FLOOR) return pm10_raw;
  float est = kPM10_CNT_K * (0.7f*gt10 + 0.2f*gt25 + 0.1f*gt50);
  if (est < kPM_MIN_FLOOR_10) est = kPM_MIN_FLOOR_10;
  return est;
}

// EMA avec dt adaptatif (en ms)
static inline float emaStep(float prev, float x, uint32_t dt_ms, float tau_sec){
  // alpha = 1 - exp(-dt/tau)
  float alpha = 1.0f - expf(-(dt_ms / 1000.0f) / tau_sec);
  if (!isfinite(prev)) return x;  // init
  return prev + alpha * (x - prev);
}

// Corrige + anti-zéro + EMA
void pmsPostProcess(const PmsData& in, float& pm1, float& pm25, float& pm10){
  // 1) affine usine
  float r1  = pmAffine((float)in.pm1_atm,  kPM1_Af,  kPM1_Bf);
  float r25 = pmAffine((float)in.pm25_atm, kPM25_Af, kPM25_Bf);
  float r10 = pmAffine((float)in.pm10_atm, kPM10_Af, kPM10_Bf);

  // 2) anti-zéro basé counts (PM2.5 & PM10 surtout)
  r25 = pmAntiZeroFromCounts25(r25, in.gt03, in.gt10, in.gt25);
  r10 = pmAntiZeroFromCounts10 (r10, in.gt10, in.gt25, in.gt50);

  // (on peut laisser PM1 sans anti-zéro, il a moins d’intérêt visuel au repos)

  // clamp sécurité
  if (r1  < 0.f) r1  = 0.f;
  if (r25 < 0.f) r25 = 0.f;
  if (r10 < 0.f) r10 = 0.f;

  // 3) EMA
  uint32_t now = millis();
  uint32_t dt  = (s_pmsFilt.lastMs==0) ? 0 : (now - s_pmsFilt.lastMs);
  s_pmsFilt.pm1  = emaStep(s_pmsFilt.pm1,  r1,  dt, kEMA_TAU_SEC);
  s_pmsFilt.pm25 = emaStep(s_pmsFilt.pm25, r25, dt, kEMA_TAU_SEC);
  s_pmsFilt.pm10 = emaStep(s_pmsFilt.pm10, r10, dt, kEMA_TAU_SEC);
  s_pmsFilt.lastMs = now;

  pm1  = s_pmsFilt.pm1;
  pm25 = s_pmsFilt.pm25;
  pm10 = s_pmsFilt.pm10;
}






static bool pmsStarted = false;
static int s_pmsSetPin = -1;
static bool pmsAlwaysOn = PMS_ALWAYS_ON;
static volatile bool s_pmsAwake = false;
void pmsInitPins(int setPin){
  s_pmsSetPin = setPin;
  pinMode(setPin, OUTPUT);
  if (pmsAlwaysOn) {
    digitalWrite(setPin, HIGH);  // allumé en permanence
    s_pmsAwake = true;
  } else {
    digitalWrite(setPin, LOW);   // tu veux le comportement ancien par défaut
    s_pmsAwake = false;
  }
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
// Échantillon bloquant : réveille, attend warmup, lit la dernière trame vue, rendort
bool pmsSampleBlocking(uint32_t warmupMs, PmsData& out){
  pmsWake();
  uint32_t t0 = millis();
  // Laisse tourner la tâche PMS qui met à jour gPms
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
bool sensorsInit(){
  Wire.begin();
  delay(100);
  Wire.setClock(100000);
  if (!aht.begin())  Serial.println("AHT21 initialization failed!");
  else               Serial.println("AHT21 initialisé avec succès.");

  if (!ens160.begin()) Serial.println("Échec ENS160 !");
  else { Serial.println("ENS160 initialisé avec succès."); ens160.setMode(ENS160_OPMODE_STD); }

  if (!gPmsMutex) gPmsMutex = xSemaphoreCreateMutex();
  return true;
}
bool safeSensorRead(float& tempC, float& humidity){
  sensors_event_t eventHum, eventTemp;
  aht.getEvent(&eventHum, &eventTemp);
  if (isnan(eventTemp.temperature) || isnan(eventHum.relative_humidity)){
    Serial.println("Erreur de lecture capteur : données invalides");
    return false;
  }

  const float T_raw = eventTemp.temperature;
  const float RH_raw = eventHum.relative_humidity;

  // 1) Correction 3 niveaux sur TEMP
  const float T_corr = calApplyTemp(T_raw);

  // 2) Compensation d'humidité par changement de T (physique)
  float RH_tc = rhTempCompensate(RH_raw, T_raw, T_corr);

  // 3) (Optionnel) Appliquer ensuite ta calibration 3 niveaux HUM
  //    Si tu ne veux pas de correction HR pour l'instant: commente la ligne suivante
  //    Sinon, garde-la (avec bornes internes).
  float RH_corr = /* calApplyHum( */ RH_tc /* ) */;

  // Clamp final (sécurité)
  if (!isfinite(RH_corr)) RH_corr = RH_tc; // fallback
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
    Serial.printf("PMS: CF1 PM1=%u PM2.5=%u PM10=%u | ATM PM1=%u PM2.5=%u PM10=%u\n",
                  out.pm1_cf1, out.pm25_cf1, out.pm10_cf1,
                  out.pm1_atm, out.pm25_atm, out.pm10_atm);
    out.valid=true; out.lastMs=millis(); return true;
  }
  return false;
}


void pmsWake(){  if (s_pmsSetPin>=0) digitalWrite(s_pmsSetPin, HIGH); s_pmsAwake=true; }
void pmsSleep(){ if (s_pmsSetPin>=0) digitalWrite(s_pmsSetPin, LOW);  s_pmsAwake=false; }

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
