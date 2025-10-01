#pragma once
#include <Arduino.h>

// Clé en base64 chargée en RAM (depuis NVS ou DEVICE_KEY_B64)
extern String g_deviceKeyB64;

// À appeler une fois au boot (avant setupBLE)
void loadOrInitDevKey();
