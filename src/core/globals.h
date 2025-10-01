#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <Adafruit_AHTX0.h>
#include "ScioSense_ENS160.h"
#include <Adafruit_NeoPixel.h>
#include <HardwareSerial.h>
#include <freertos/semphr.h>

// === LED modes (même noms qu'avant)
enum LedMode { LED_BOOT, LED_PAIRING, LED_GOOD, LED_MODERATE, LED_BAD, LED_UPDATING, LED_OFF };

// === PMS
struct PmsData {
  uint16_t pm1_cf1=0, pm25_cf1=0, pm10_cf1=0;
  uint16_t pm1_atm=0, pm25_atm=0, pm10_atm=0;
  uint16_t gt03=0, gt05=0, gt10=0, gt25=0, gt50=0, gt100=0;
  bool     valid=false;
  uint32_t lastMs=0;
  uint32_t seq=0;
};
enum WifiAuthType : uint8_t {
  WIFI_CONN_PSK = 0,                  // SSID + password
  WIFI_CONN_EAP_PEAP_MSCHAPV2 = 1     // Enterprise (PEAP/MSCHAPv2 + CA)
};
// globals.h ou wifi_mqtt.cpp (statiques)
static volatile bool g_factoryResetPending = false;
// ==== GLOBAUX (définis dans globals.cpp) ====
extern bool wifiConnected;
extern bool needToConnectWiFi;
extern volatile bool otaInProgress;
extern unsigned long lastOtaCheck;
extern uint8_t      wifiFailCount;
extern unsigned long lastWifiAttemptMs;
extern bool         bleInited;
extern Preferences prefs;
extern String wifiSSID, wifiPassword, sensorId, userId;

extern WiFiClientSecure tlsClient;
extern PubSubClient     mqttClient;

extern Adafruit_AHTX0   aht;
extern ScioSense_ENS160 ens160;

extern HardwareSerial& PMS;
extern PmsData gPms;
extern SemaphoreHandle_t gPmsMutex;

extern Adafruit_NeoPixel led;
extern volatile LedMode currentLedMode;
extern volatile bool    ledOverride;

extern unsigned long lastPublish;

uint16_t be16(const uint8_t *b);

// ==== Ajout: sélection d'auth + credentials EAP ====
extern WifiAuthType wifiAuthType;
extern String eapIdentity;   // inner identity (souvent = username) (optionnel)
extern String eapUsername;   // inner username (MSCHAPv2)
extern String eapPassword;   // inner password
extern String eapAnon;       // outer anonymous identity (ex: "ano@rezoleo.fr")

// ==== API réseau ====
bool connectToWiFi();               // route PSK/EAP
bool connectToWiFiEnterprise();     // EAP-PEAP/MSCHAPv2 + CA
void startSNTPAfterConnected();     // SNTP après Wi-Fi connecté

// ==== Helpers capteurs (déjà existants chez toi) ====
bool safeSensorRead(float& t, float& h);
void sensorsReadEns160(int& aqi,int& tvoc,int& eco2,float t,float h);

// (updateLedState est ailleurs chez toi)
