#pragma once
#include <Arduino.h>

/** SCD41 : CO2 NDIR réel (ppm), température (°C), humidité (%).  
 *  Init via scd41Init() (appelé depuis sensorsInit()).  
 *  startPeriodicMeasurement() lancé à l’init ; lecture via scd41Read(). */

/** Initialise le SCD41 (I2C) et démarre la mesure périodique.  
 *  À appeler après Wire.begin(). Retourne true si le capteur répond. */
bool scd41Init(void);

/** Lit la dernière mesure (CO2, T, HR). Retourne true si lecture OK.  
 *  co2Ppm : CO2 en ppm (uint16_t)  
 *  tempC : température en °C  
 *  humidity : humidité relative en % (0–100) */
bool scd41Read(uint16_t& co2Ppm, float& tempC, float& humidity);
