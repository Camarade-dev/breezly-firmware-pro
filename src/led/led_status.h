#pragma once
#include "../core/globals.h"

void ledInit(uint8_t pin, uint8_t count);
void updateLedState(LedMode mode);
void ledTaskStart();

// NEW
void ledSuspend();   // coupe l’anim/show RMT pendant l’OTA
void ledResume();    // relance après OTA
bool ledIsMuted();   // (optionnel)
void ledNotifyPublish();
void ledSetAirQualityScore(float score01);