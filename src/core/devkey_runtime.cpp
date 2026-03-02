#include <Preferences.h>
#include "devkey_runtime.h"
#include "devkey.h"   // <- généré par le pre-build (#define DEVICE_KEY_B64 "...")
#include "log.h"
#ifdef DEVICE_KEY_B64
#warning [DEVKEY] macro DEVICE_KEY_B64 is PRESENT at compile time
static const char* kDevConstProbe = DEVICE_KEY_B64;
#else
#warning [DEVKEY] macro DEVICE_KEY_B64 is MISSING at compile time
#endif

String g_deviceKeyB64;

void loadOrInitDevKey() {
  Preferences p;

  String nvsKey;
  if (p.begin("myApp", true)) {
    nvsKey = p.getString("devKey", "");
    p.end();
  }

#ifdef DEVICE_KEY_B64
  const String buildKey = String(DEVICE_KEY_B64);
#else
  const String buildKey = "";
#endif

#if defined(FORCE_DEVKEY_FROM_BUILD) && defined(DEVICE_KEY_B64)
  // En mode factory: toujours imposer la clé du build dans la NVS
  g_deviceKeyB64 = buildKey;
  if (p.begin("myApp", false)) { p.putString("devKey", g_deviceKeyB64); p.end(); }
  LOGD("DEVKEY", "FORCED from build, len=%d", (int)g_deviceKeyB64.length());
  return;
#endif

  // Mode normal: si NVS a une clé, on l'utilise, sinon on injecte celle du build.
  if (nvsKey.length() > 2) { g_deviceKeyB64 = nvsKey; LOGD("DEVKEY", "loaded from NVS"); return; }

#ifdef DEVICE_KEY_B64
  g_deviceKeyB64 = buildKey;
  if (g_deviceKeyB64.length() > 0) {
    if (p.begin("myApp", false)) { p.putString("devKey", g_deviceKeyB64); p.end(); }
    LOGD("DEVKEY", "stored build key into NVS");
  }
#else
  LOGW("DEVKEY", "no key available (no NVS, no build macro)");
#endif
}

