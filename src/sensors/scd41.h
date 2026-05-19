#pragma once
#include <Arduino.h>
#include <stdint.h>

/** SCD41 : CO2 NDIR réel (ppm), température (°C), humidité (%).
 *  Init via scd41Init() (appelé depuis sensorsInit()).
 *  startPeriodicMeasurement() lancé à l'init ; lecture via scd41Read(). */

/** Diagnostics SCD41 (lecture seule, pour télémétrie / logs).
 *  Tous les champs sont mis à jour depuis le contexte loop (pas d'ISR). */
struct Scd41Diag {
  bool     initialized = false;       // état courant
  uint32_t initAttempts = 0;          // nb d'appels scd41Init()
  uint32_t stopRecoveryCount = 0;     // nb de stopPeriodicMeasurement() émis
  bool     lastUsedStopRecovery = false; // le dernier init a-t-il dû stop+retry ?
  uint16_t lastInitErr = 0;           // err startPeriodicMeasurement() du dernier essai
  uint32_t readFailures = 0;          // nb d'échecs readMeasurement()
};

/** Initialise le SCD41 (I2C) et démarre la mesure périodique.
 *  Idempotent et sûr après reboot à chaud : émet d'abord
 *  stopPeriodicMeasurement() (le SCD4x refuse start_periodic_measurement
 *  s'il est déjà en mesure périodique d'une session précédente), attend le
 *  délai requis (500 ms géré par la lib), puis (re)démarre — essais bornés.
 *  À appeler après Wire.begin(). Retourne true si le capteur répond. */
bool scd41Init(void);

/** Init paresseuse : init seulement si pas déjà initialisé.
 *  Retourne true si le capteur est prêt. */
bool scd41EnsureInitialized(void);

/** Lit la dernière mesure (CO2, T, HR). Retourne true si lecture OK.
 *  Si non initialisé, retente une init périodiquement (non bloquant côté
 *  cadence, throttlé) : un échec d'init n'est jamais définitif pour la session.
 *  co2Ppm : CO2 en ppm (uint16_t)
 *  tempC : température en °C
 *  humidity : humidité relative en % (0–100) */
bool scd41Read(uint16_t& co2Ppm, float& tempC, float& humidity);

/** true si le SCD41 est actuellement initialisé. */
bool scd41IsInitialized(void);

/** Copie l'état diagnostic courant. */
void scd41GetDiag(Scd41Diag& out);
