#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Handle incoming MQTT message on /control topic. Call from mqtt_bus onMqttMessage. */
void mqttCtrlHandleMessage(const char* topic, const uint8_t* payload, unsigned int len);

#ifdef __cplusplus
}
#endif
