#pragma once
#include <Arduino.h>

// Démarre (ou prépare) le serveur BLE. Si startAdvertising=true, lance la pub tout de suite.
void setupBLE(bool startAdvertising);

// Relance proprement l’advertising (après échec Wi-Fi par ex).
void restartBLEAdvertising();
void provisioningSetStatus(const char* json);
void provisioningNotifyConnected();
// Indique si la pile/serveur BLE a été initialisé au moins une fois.
extern bool bleInited;
