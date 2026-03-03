#pragma once
#include <Arduino.h>
#include "../core/globals.h"

bool sensorsInit();
/** Lit T et HR ; remplit tempC/humidity (calibrés). Si outRawTempC/outRawHumidity non null, y met les valeurs brutes (sans calibration). */
bool safeSensorRead(float& tempC, float& humidity, float* outRawTempC, float* outRawHumidity);
void sensorsReadEns160(int& aqi, int& tvoc, int& eco2, float tempC, float humidity);

/** Sanity check AQI/TVOC/eCO2 (seuils dans app_config.h). Ne bloque jamais l'envoi.
 *  Retourne true si toutes les valeurs sont dans les plages considérées "sanes".
 *  Si failOut != nullptr et failOutSize > 0, écrit en cas d'échec la liste des champs hors plage ("aqi", "tvoc", "eco2", séparés par des virgules). */
bool sensorSanityCheck(int aqi, int tvoc, int eco2, char* failOut, size_t failOutSize);

void pmsTaskStart(int rx, int tx);
void  pmsInitPins(int setPin);
void  pmsSleep();                 // SET=LOW
void  pmsWake();                  // SET=HIGH
bool  pmsSampleBlocking(uint32_t warmupMs, PmsData& out); // wake->attend->lit->sleep
void pmsPostProcess(const PmsData& in, float& pm1, float& pm25, float& pm10);