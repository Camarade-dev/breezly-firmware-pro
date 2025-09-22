#include "crc_utils.h"
#include "esp_crc.h"
#include "../core/globals.h"

uint32_t computeChecksum(const String& ssid, const String& pwd, const String& sensor, const String& user) {
  String full = ssid + "|" + pwd + "|" + sensor + "|" + user;
  return esp_crc32_le(0, (const uint8_t*)full.c_str(), full.length());
}

bool preferencesAreValid() {
  prefs.begin("myApp", true);
  String ssid  = prefs.getString("wifiSSID", "");
  String pwd   = prefs.getString("wifiPassword", "");
  String sid   = prefs.getString("sensorId", "");
  String uid   = prefs.getString("userId", "");
  uint32_t storedChecksum = prefs.getUInt("checksum", 0);
  prefs.end();
  uint32_t computed = computeChecksum(ssid, pwd, sid, uid);
  return storedChecksum == computed;
}
