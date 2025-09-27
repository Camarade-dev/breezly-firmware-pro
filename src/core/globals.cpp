#include "globals.h"
#include <Adafruit_NeoPixel.h>
#include "../app_config.h"
bool wifiConnected = false;
bool needToConnectWiFi = false;
volatile bool otaInProgress = false;
unsigned long lastOtaCheck = 0;
uint8_t       wifiFailCount = 0;
unsigned long lastWifiAttemptMs = 0;
bool          bleInited = false;
Preferences prefs;
String wifiSSID = "";
String wifiPassword = "";
String sensorId = "";
String userId = "";

WiFiClientSecure tlsClient;
PubSubClient     mqttClient(tlsClient);

Adafruit_AHTX0   aht;
ScioSense_ENS160 ens160(0x52);

HardwareSerial& PMS = Serial2;
PmsData gPms;
SemaphoreHandle_t gPmsMutex = nullptr;

Adafruit_NeoPixel led(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
volatile LedMode currentLedMode = LED_BOOT;
volatile bool    ledOverride = false;

unsigned long lastPublish = 0;

uint16_t be16(const uint8_t *b){ return (uint16_t)b[0]<<8 | b[1]; }
WifiAuthType wifiAuthType = WIFI_CONN_PSK;  // défaut: PSK

String eapIdentity = "";
String eapUsername = "";
String eapPassword = "";
String eapAnon     = "ano@rezoleo.fr";