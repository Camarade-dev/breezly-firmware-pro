#pragma once
#include <Arduino.h>
#include "../core/globals.h"

bool sensorsInit();
bool safeSensorRead(float& tempC, float& humidity);
void sensorsReadEns160(int& aqi, int& tvoc, int& eco2, float tempC, float humidity);

void pmsTaskStart(int rx, int tx);
void  pmsInitPins(int setPin);
void  pmsSleep();                 // SET=LOW
void  pmsWake();                  // SET=HIGH
bool  pmsSampleBlocking(uint32_t warmupMs, PmsData& out); // wake->attend->lit->sleep