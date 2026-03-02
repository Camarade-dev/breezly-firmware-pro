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
void otaSetInProgress(bool v);

// Pending update: à appeler par l’app quand le device est sain (ex. après MQTT connect + première télémétrie)
bool otaIsPendingUpdate();
void otaMarkAppValidIfPending();