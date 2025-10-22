#include "sensors.h"
#include <Wire.h>
#include "../app_config.h"
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
  tempC = eventTemp.temperature;
  humidity = eventHum.relative_humidity;
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
