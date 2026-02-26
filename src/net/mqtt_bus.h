#pragma once
#include <Arduino.h>

// ========= Message de publication =========
// AVANT
// struct PubMsg { String topic; String payload; uint8_t qos; bool retain; };

// APRÈS
struct PubMsg {
  char*   topic;
  char*   payload;
  uint8_t qos;
  bool    retain;
};


// ========= API publique =========
void mqtt_bus_start_task();                       // à appeler dans setup()
void mqtt_request_connect();                      // demander (re)connexion
bool mqtt_is_connected();                         // lire l'état
bool mqtt_enqueue(const String& topic,
                  const String& payload,
                  uint8_t qos = 0,
                  bool retain = false);           // producteurs -> queue
bool mqtt_flush(uint32_t timeout_ms = 1000);      // attendre queue vide (optionnel)

// Helpers pour topics device
String mqtt_topic_device_base();                  // "breezly/devices/{sensorId}"
String mqtt_topic_ota();                          // base + "/ota"
String mqtt_topic_ctrl();                         // base + "/control"
String mqtt_topic_status();                       // base + "/status"
String mqtt_topic_telemetry();                    // base + "/telemetry"

// Emit telemetry event (enqueued; payload: type, fw_version, device_id, ts, context)
bool mqtt_telemetry_emit(const char* type, const char* context_json);
