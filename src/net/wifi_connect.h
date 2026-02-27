#pragma once
#include <Arduino.h>

// Essaie de connecter le Wi-Fi selon wifiAuthType (PSK ou EAP).
// Retourne true si connecté (WiFi.status()==WL_CONNECTED).
bool connectToWiFi();

// --- Backoff exponentiel (retry Wi‑Fi) ---
void wifiBackoffReset();
bool wifiBackoffShouldAttempt();
