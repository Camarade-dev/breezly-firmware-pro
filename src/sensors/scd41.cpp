// src/sensors/scd41.cpp — SCD41 CO2 NDIR + température + humidité
#include "scd41.h"
#include <Wire.h>
#include "SensirionI2CScd4x.h"
#include "../core/log.h"

static SensirionI2CScd4x s_scd4x;
static bool s_initialized = false;

bool scd41Init(void)
{
    s_scd4x.begin(Wire);
    uint16_t err = s_scd4x.startPeriodicMeasurement();
    if (err != 0)
    {
        LOGW("SCD41", "startPeriodicMeasurement failed err=%u", (unsigned)err);
        s_initialized = false;
        return false;
    }
    s_initialized = true;
    LOGD("SCD41", "initialized, periodic measurement started");
    return true;
}

bool scd41Read(uint16_t& co2Ppm, float& tempC, float& humidity)
{
    if (!s_initialized)
        return false;

    uint16_t co2;
    float temperature;
    float humidityPct;
    if (s_scd4x.readMeasurement(co2, temperature, humidityPct) != 0)
        return false;

    co2Ppm = co2;
    tempC = temperature;
    humidity = humidityPct;
    return true;
}
