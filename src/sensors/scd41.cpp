// src/sensors/scd41.cpp — SCD41 CO2 NDIR + température + humidité
#include "scd41.h"
#include <Wire.h>
#include "SensirionI2CScd4x.h"
#include "../core/log.h"

// Essais d'init : 1 essai direct + (N-1) essais après stop+retry.
#ifndef SCD41_INIT_MAX_ATTEMPTS
#define SCD41_INIT_MAX_ATTEMPTS   (3)
#endif
// Période mini entre deux tentatives de ré-init depuis le chemin de lecture.
#ifndef SCD41_REINIT_RETRY_MS
#define SCD41_REINIT_RETRY_MS     (30000UL)
#endif

static SensirionI2CScd4x s_scd4x;
static bool s_initialized = false;
static uint32_t s_lastInitMs = 0;
static Scd41Diag s_diag;

bool scd41Init(void)
{
    s_scd4x.begin(Wire);
    s_lastInitMs = millis();
    s_diag.initAttempts++;

    uint16_t err = 0;
    bool usedStopRecovery = false;

    for (int attempt = 0; attempt < SCD41_INIT_MAX_ATTEMPTS; attempt++)
    {
        // Idempotent / sûr après reboot à chaud : le SCD4x refuse
        // start_periodic_measurement (NACK -> WriteError|I2cAddressNack)
        // s'il est DÉJÀ en mesure périodique d'une session précédente
        // (rail/capteur non coupé). On le repasse d'abord en idle.
        // stopPeriodicMeasurement() applique en interne le délai de 500 ms
        // requis par le datasheet (et n'échoue pas si déjà en idle).
        (void)s_scd4x.stopPeriodicMeasurement();
        s_diag.stopRecoveryCount++;
        usedStopRecovery = true;

        err = s_scd4x.startPeriodicMeasurement();
        if (err == 0)
            break;

        LOGW("SCD41", "startPeriodicMeasurement failed err=%u (attempt %d/%d)",
             (unsigned)err, attempt + 1, SCD41_INIT_MAX_ATTEMPTS);
    }

    s_diag.lastInitErr = err;
    s_diag.lastUsedStopRecovery = usedStopRecovery;

    if (err != 0)
    {
        LOGW("SCD41", "init failed after %d attempts (err=%u)",
             SCD41_INIT_MAX_ATTEMPTS, (unsigned)err);
        s_initialized = false;
        s_diag.initialized = false;
        return false;
    }

    s_initialized = true;
    s_diag.initialized = true;
    LOGI("SCD41", "initialized, periodic measurement started%s",
         usedStopRecovery ? " (stop+retry recovery)" : "");
    return true;
}

bool scd41EnsureInitialized(void)
{
    if (s_initialized)
        return true;
    return scd41Init();
}

bool scd41Read(uint16_t& co2Ppm, float& tempC, float& humidity)
{
    if (!s_initialized)
    {
        // Un échec d'init n'est jamais définitif pour la session : on retente
        // périodiquement depuis le chemin de lecture (throttlé, pas de log
        // en boucle, indépendant de la santé AHT21).
        uint32_t now = millis();
        if ((uint32_t)(now - s_lastInitMs) < SCD41_REINIT_RETRY_MS)
            return false;
        if (!scd41Init())
            return false;
    }

    uint16_t co2;
    float temperature;
    float humidityPct;
    if (s_scd4x.readMeasurement(co2, temperature, humidityPct) != 0)
    {
        s_diag.readFailures++;
        return false;
    }

    co2Ppm = co2;
    tempC = temperature;
    humidity = humidityPct;
    return true;
}

bool scd41IsInitialized(void)
{
    return s_initialized;
}

void scd41GetDiag(Scd41Diag& out)
{
    out = s_diag;
}
