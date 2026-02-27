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

// Installation UX (priorité haute, non écrasée par pulse/air)
void ledOnBoot();              // bleu + timer silent 2 min
void ledOnProvisioningStart(); // annule silent, jaune 1 Hz
void ledOnProvisioningError(); // rouge 4 Hz
void ledOnConnectedOk();       // vert 3 s puis OFF, installFinished

// Night mode (anti-éblouissement) : état lisible + override pilotable via MQTT
bool ledGetNightMode();        // true si mode nuit actif (pour télémétrie)
void ledSetNightModeOverride(int v);  // 0=auto, 1=forcé nuit, 2=forcé jour