#pragma once
#include "../core/globals.h"

void ledInit(uint8_t pin, uint8_t count);
void updateLedState(LedMode mode);
void ledTaskStart();
