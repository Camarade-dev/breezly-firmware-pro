#pragma once

/** BMP581 : pression (Pa) et température (°C).
 *  Init via bmp581Init() (appelé depuis sensorsInit()).
 *  Lecture non bloquante via bmp581Read(). */

/** Initialise le BMP581 (I2C). À appeler après Wire.begin().
 *  Retourne true si le capteur répond et est configuré. */
bool bmp581Init(void);

/** Lit pression et température. Retourne true si lecture OK.
 *  pressurePa : pression en pascals (Pa)
 *  tempC : température en °C */
bool bmp581Read(float& pressurePa, float& tempC);
