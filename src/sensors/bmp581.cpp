// src/sensors/bmp581.cpp — BMP581 barometric pressure + temperature
#include "bmp581.h"
#include <Arduino.h>
#include <Wire.h>
#include "bmp5.h"
#include "../core/log.h"

#define BMP_I2C_ADDR 0x47

// Essais d'init : 1 essai direct + (N-1) essais après soft-reset.
#ifndef BMP581_INIT_MAX_ATTEMPTS
#define BMP581_INIT_MAX_ATTEMPTS     (3)
#endif
// Marge au-delà du délai soft-reset Bosch (BMP5_DELAY_US_SOFT_RESET = 2 ms).
#ifndef BMP581_SOFT_RESET_SETTLE_MS
#define BMP581_SOFT_RESET_SETTLE_MS  (5)
#endif
// Période mini entre deux tentatives de ré-init depuis le chemin de lecture.
#ifndef BMP581_REINIT_RETRY_MS
#define BMP581_REINIT_RETRY_MS       (30000UL)
#endif

static bmp5_dev s_dev;
static bmp5_osr_odr_press_config s_osr_odr_press_cfg;
static bmp5_iir_config s_iir_cfg;
static bool s_initialized = false;
static uint32_t s_lastInitMs = 0;
static Bmp581Diag s_diag;

static int8_t i2c_read(uint8_t reg_addr, uint8_t *data, uint32_t len, void *intf_ptr)
{
    (void)intf_ptr;
    Wire.beginTransmission(BMP_I2C_ADDR);
    Wire.write(reg_addr);
    if (Wire.endTransmission(false) != 0)
        return -1;
    if (Wire.requestFrom(BMP_I2C_ADDR, len) != (size_t)len)
        return -1;
    for (uint32_t i = 0; i < len && Wire.available(); i++)
        data[i] = Wire.read();
    return 0;
}

static int8_t i2c_write(uint8_t reg_addr, const uint8_t *data, uint32_t len, void *intf_ptr)
{
    (void)intf_ptr;
    Wire.beginTransmission(BMP_I2C_ADDR);
    Wire.write(reg_addr);
    for (uint32_t i = 0; i < len; i++)
        Wire.write(data[i]);
    return (Wire.endTransmission() == 0) ? 0 : -1;
}

static void delay_us_cb(uint32_t period, void *intf_ptr)
{
    (void)intf_ptr;
    delayMicroseconds(period);
}

static void bmp581BindDev(void)
{
    s_dev.intf = BMP5_I2C_INTF;
    s_dev.read = i2c_read;
    s_dev.write = i2c_write;
    s_dev.delay_us = delay_us_cb;
    s_dev.intf_ptr = NULL;
}

// Soft-reset "brut" : on écrit nous-mêmes la commande reset puis on attend.
// On N'UTILISE PAS bmp5_soft_reset() : ce wrapper Bosch relit INT_STATUS pour
// vérifier la fin du reset (bmp5.c:447), ce qui CONSOMME le bit clear-on-read
// POR_SOFTRESET_COMPLETE. Or bmp5_init() exige ensuite ce même bit
// (power_up_check, bmp5.c:1487) : il échouerait avec BMP5_E_POWER_UP.
// En écrivant la commande sans relire INT_STATUS, le bit reste armé et
// le bmp5_init() qui suit le trouve « frais » et réussit.
// C'est exactement le cas du reboot à chaud (ESP.restart/OTA/panic) : le
// BMP581 reste alimenté et le bit POR a été consommé par la session
// précédente, donc le premier bmp5_init() échoue tant qu'on n'a pas reset.
static bool bmp581SoftResetRaw(void)
{
    const uint8_t cmd = BMP5_SOFT_RESET_CMD;
    if (i2c_write(BMP5_REG_CMD, &cmd, 1, NULL) != 0)
        return false;
    delay(BMP581_SOFT_RESET_SETTLE_MS); // >= BMP5_DELAY_US_SOFT_RESET (2 ms)
    return true;
}

static bool bmp581ApplyConfig(void)
{
    s_osr_odr_press_cfg.press_en = BMP5_ENABLE;
    s_osr_odr_press_cfg.osr_t = BMP5_OVERSAMPLING_1X;
    s_osr_odr_press_cfg.osr_p = BMP5_OVERSAMPLING_8X;
    s_osr_odr_press_cfg.odr = BMP5_ODR_01_HZ;
    if (bmp5_set_osr_odr_press_config(&s_osr_odr_press_cfg, &s_dev) != BMP5_OK)
    {
        LOGW("BMP581", "set osr/odr failed");
        return false;
    }

    s_iir_cfg.set_iir_t = BMP5_IIR_FILTER_COEFF_3;
    s_iir_cfg.set_iir_p = BMP5_IIR_FILTER_COEFF_3;
    if (bmp5_set_iir_config(&s_iir_cfg, &s_dev) != BMP5_OK)
    {
        LOGW("BMP581", "set iir failed");
        return false;
    }

    if (bmp5_set_power_mode(BMP5_POWERMODE_NORMAL, &s_dev) != BMP5_OK)
    {
        LOGW("BMP581", "set power mode failed");
        return false;
    }
    return true;
}

bool bmp581Init(void)
{
    bmp581BindDev();
    s_lastInitMs = millis();
    s_diag.initAttempts++;

    int8_t rslt = BMP5_E_POWER_UP;
    bool usedSoftReset = false;

    for (int attempt = 0; attempt < BMP581_INIT_MAX_ATTEMPTS; attempt++)
    {
        rslt = bmp5_init(&s_dev);
        if (rslt == BMP5_OK)
            break;

        // Échec typique sur reboot à chaud : bit POR consommé par la session
        // précédente. Un soft-reset brut le réarme pour l'essai suivant.
        if (attempt + 1 < BMP581_INIT_MAX_ATTEMPTS)
        {
            usedSoftReset = true;
            s_diag.softResetCount++;
            if (!bmp581SoftResetRaw())
                LOGW("BMP581", "soft-reset write failed");
        }
    }

    s_diag.lastInitRc = (int)rslt;
    s_diag.lastUsedSoftReset = usedSoftReset;

    if (rslt != BMP5_OK)
    {
        LOGW("BMP581", "init failed after %d attempts (rc=%d, softReset=%d)",
             BMP581_INIT_MAX_ATTEMPTS, (int)rslt, (int)usedSoftReset);
        s_initialized = false;
        s_diag.initialized = false;
        return false;
    }

    if (!bmp581ApplyConfig())
    {
        s_initialized = false;
        s_diag.initialized = false;
        return false;
    }

    s_initialized = true;
    s_diag.initialized = true;
    LOGI("BMP581", "initialized%s", usedSoftReset ? " (soft-reset recovery)" : "");
    return true;
}

bool bmp581EnsureInitialized(void)
{
    if (s_initialized)
        return true;
    return bmp581Init();
}

bool bmp581Read(float& pressurePa, float& tempC)
{
    if (!s_initialized)
    {
        // Un échec d'init n'est jamais définitif pour la session : on retente
        // périodiquement depuis le chemin de lecture (non bloquant, throttlé,
        // pas de log en boucle).
        uint32_t now = millis();
        if ((uint32_t)(now - s_lastInitMs) < BMP581_REINIT_RETRY_MS)
            return false;
        if (!bmp581Init())
            return false;
    }

    bmp5_sensor_data data;
    if (bmp5_get_sensor_data(&data, &s_osr_odr_press_cfg, &s_dev) != BMP5_OK)
    {
        s_diag.readFailures++;
        return false;
    }

    pressurePa = (float)data.pressure;
    tempC = (float)data.temperature;
    return true;
}

bool bmp581IsInitialized(void)
{
    return s_initialized;
}

void bmp581GetDiag(Bmp581Diag& out)
{
    out = s_diag;
}
