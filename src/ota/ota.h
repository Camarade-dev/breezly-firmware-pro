#pragma once
#include <Arduino.h>

bool httpDownloadToUpdate(const String& binUrl);
void checkAndPerformCloudOTA();
