#pragma once
#include <Arduino.h>

bool connectToWiFi();
bool connectToMQTT();
void mqttSubscribeOtaTopic();
void mqttLoopOnce();
void scheduleMqttConnect();