#pragma once
#include <Arduino.h>

// Appelle ceci tôt dans setup() (après Serial.begin et avant services réseau)
void otaOnBootValidate();

// Check périodique (tu peux l’appeler depuis ta loop ou un Task dédié)
void checkAndPerformCloudOTA();

// Optionnel: pour forcer un check (ex. via MQTT)
void triggerOtaCheckNow();

// (exposé si tu veux afficher l’état)
bool otaIsInProgress();
