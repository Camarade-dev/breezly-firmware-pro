// src/sensors/bmp581.cpp — BMP581 barometric pressure + temperature
#include "bmp581.h"
#include <Wire.h>
#include "bmp5.h"
#include "../core/log.h"

#define BMP_I2C_ADDR 0x47

static bmp5_dev s_dev;
static bmp5_osr_odr_press_config s_osr_odr_press_cfg;
static bmp5_iir_config s_iir_cfg;
static bool s_initialized = false;

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

bool bmp581Init(void)
{
    s_dev.intf = BMP5_I2C_INTF;
    s_dev.read = i2c_read;
    s_dev.write = i2c_write;
    s_dev.delay_us = delay_us_cb;
    s_dev.intf_ptr = NULL;

    if (bmp5_init(&s_dev) != BMP5_OK)
    {
        LOGW("BMP581", "init failed");
        s_initialized = false;
        return false;
    }

    s_osr_odr_press_cfg.press_en = BMP5_ENABLE;
    s_osr_odr_press_cfg.osr_t = BMP5_OVERSAMPLING_1X;
    s_osr_odr_press_cfg.osr_p = BMP5_OVERSAMPLING_8X;
    s_osr_odr_press_cfg.odr = BMP5_ODR_01_HZ;

    if (bmp5_set_osr_odr_press_config(&s_osr_odr_press_cfg, &s_dev) != BMP5_OK)
    {
        LOGW("BMP581", "set osr/odr failed");
        s_initialized = false;
        return false;
    }

    s_iir_cfg.set_iir_t = BMP5_IIR_FILTER_COEFF_3;
    s_iir_cfg.set_iir_p = BMP5_IIR_FILTER_COEFF_3;
    if (bmp5_set_iir_config(&s_iir_cfg, &s_dev) != BMP5_OK)
    {
        LOGW("BMP581", "set iir failed");
        s_initialized = false;
        return false;
    }

    if (bmp5_set_power_mode(BMP5_POWERMODE_NORMAL, &s_dev) != BMP5_OK)
    {
        LOGW("BMP581", "set power mode failed");
        s_initialized = false;
        return false;
    }

    s_initialized = true;
    LOGD("BMP581", "initialized");
    return true;
}

bool bmp581Read(float& pressurePa, float& tempC)
{
    if (!s_initialized)
        return false;

    bmp5_sensor_data data;
    if (bmp5_get_sensor_data(&data, &s_osr_odr_press_cfg, &s_dev) != BMP5_OK)
        return false;

    pressurePa = (float)data.pressure;
    tempC = (float)data.temperature;
    return true;
}
