#pragma once
#include <stdint.h>

/** BMP581 : pression (Pa) et température (°C).
 *  Init via bmp581Init() (appelé depuis sensorsInit()).
 *  Lecture non bloquante via bmp581Read(). */

/** Diagnostics BMP581 (lecture seule, pour télémétrie / logs).
 *  Tous les champs sont mis à jour depuis le contexte loop (pas d'ISR). */
struct Bmp581Diag {
  bool     initialized = false;       // état courant
  uint32_t initAttempts = 0;          // nb d'appels bmp581Init()
  uint32_t softResetCount = 0;        // nb de soft-resets émis (recovery)
  bool     lastUsedSoftReset = false; // le dernier init a-t-il dû reset ?
  int      lastInitRc = 0;            // code retour bmp5_init() du dernier essai
  uint32_t readFailures = 0;          // nb d'échecs bmp5_get_sensor_data()
};

/** Initialise le BMP581 (I2C). À appeler après Wire.begin().
 *  Idempotent et sûr après reboot à chaud : tente bmp5_init(), et en cas
 *  d'échec émet un soft-reset puis réessaie (nombre d'essais borné).
 *  Retourne true si le capteur répond et est configuré. */
bool bmp581Init(void);

/** Init paresseuse : init seulement si pas déjà initialisé.
 *  Retourne true si le capteur est prêt. */
bool bmp581EnsureInitialized(void);

/** Lit pression et température. Retourne true si lecture OK.
 *  Si non initialisé, retente une init périodiquement (non bloquant,
 *  throttlé) : un échec d'init n'est jamais définitif pour la session.
 *  pressurePa : pression en pascals (Pa)
 *  tempC : température en °C */
bool bmp581Read(float& pressurePa, float& tempC);

/** true si le BMP581 est actuellement initialisé. */
bool bmp581IsInitialized(void);

/** Copie l'état diagnostic courant. */
void bmp581GetDiag(Bmp581Diag& out);
