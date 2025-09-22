#pragma once
#include <Arduino.h>

uint32_t computeChecksum(const String& ssid, const String& pwd, const String& sensor, const String& user);
bool preferencesAreValid();
