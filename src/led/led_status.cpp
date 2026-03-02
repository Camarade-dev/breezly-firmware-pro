#include "led_status.h"
#include <Arduino.h>
#include <time.h>
#include <Preferences.h>
#include "../core/globals.h"    // led, currentLedMode, ledOverride
#include "../net/sntp_utils.h"  // timeIsSaneHard() pour night mode

// Tâche RTOS
static TaskHandle_t s_ledTask   = nullptr;
static volatile bool s_ledMuted = false;

// --- PULSE sur publish capteurs ---
static volatile bool s_pulseRequested = false;
static volatile float s_airScore01 = 0.0f;
// 0.0 → 1.0 sur la durée d’une impulsion
static float s_pulseT = 1.0f;          
static const uint32_t PULSE_DURATION_MS = 1800;  // durée totale de la pulsation

// --- Night mode (anti-éblouissement) : fenêtre 22:30 → 07:30 heure locale ;
//     jour max 60/255, nuit max 4/255 ; si heure inconnue → mode nuit par défaut (safe sommeil).
static const int NIGHT_START_HOUR = 22, NIGHT_START_MIN = 30;
static const int NIGHT_END_HOUR = 7, NIGHT_END_MIN = 30;
static const uint8_t BRIGHTNESS_DAY = 60, BRIGHTNESS_NIGHT = 4;

// Override pilotable via MQTT : 0=auto (heure), 1=forcé nuit, 2=forcé jour
static volatile int s_nightModeOverride = 0;
static volatile bool s_nightModeActive = true;  // état actuel (pour ledGetNightMode / télémétrie)

static bool isNightNow();  // défini plus bas (helpers)

// Phase pour le "idle breathing" très léger
static float s_idlePhase = 0.0f;

// Phase dédiée au dégradé BLE pairing (rotation de teinte)
static float s_pairPhase = 0.0f;
// Phase respiration lente pour BOOT (vague douce, pas bip)
static float s_bootPhase = 0.0f;
static const float BOOT_BREATHE_PERIOD_MS = 4000.0f;

// --- Installation + priorité (minimal, non bloquant) ---
static volatile bool s_installFinished = false;
static volatile bool s_userActivitySeen = false;
static volatile uint32_t s_silentDeadlineMs = 0;
static volatile uint32_t s_highPriorityUntilMs = 0;
static volatile bool s_connectedConfirmPending = false;
static const uint32_t SILENT_TIMEOUT_MS = 120000;
static const uint32_t CONNECTED_CONFIRM_MS = 3000;

// ------------------------------------------------------------------
// Init
// ------------------------------------------------------------------
void ledInit(uint8_t, uint8_t){
  Preferences prefs;
  if (prefs.begin("myApp", true)) {
    s_nightModeOverride = (int)prefs.getUChar("nightMode", 0);
    if (s_nightModeOverride < 0 || s_nightModeOverride > 2) s_nightModeOverride = 0;
    prefs.end();
  }
  bool night = (s_nightModeOverride == 0) ? isNightNow() : (s_nightModeOverride == 1);
  s_nightModeActive = night;
  led.begin();
  led.setBrightness(night ? BRIGHTNESS_NIGHT : BRIGHTNESS_DAY);
  led.show();
}

bool ledGetNightMode() {
  return s_nightModeActive;
}

void ledSetNightModeOverride(int v) {
  if (v < 0 || v > 2) return;
  s_nightModeOverride = v;
  Preferences prefs;
  if (prefs.begin("myApp", false)) {
    prefs.putUChar("nightMode", (uint8_t)v);
    prefs.end();
  }
}

bool ledIsMuted(){ 
  return s_ledMuted; 
}

// Coupe toute activité LED (aucun show RMT pendant TLS/OTA)
void ledSuspend(){
  s_ledMuted = true;
  if (s_ledTask) vTaskSuspend(s_ledTask);
}

// Relance l’anim LED après l’OTA (si pas de reboot)
void ledResume(){
  if (s_ledTask) vTaskResume(s_ledTask);
  s_ledMuted = false;
}

// Pas de changement : tu peux toujours surcharger via ledOverride
static bool isHighPriorityActive(){
  uint32_t now = millis();
  if (s_highPriorityUntilMs != 0 && now < s_highPriorityUntilMs) return true;
  if (s_installFinished) return false;
  LedMode m = currentLedMode;
  return (m == LED_BOOT || m == LED_PAIRING || m == LED_BAD || m == LED_UPDATING);
}

static void enterHighPriority(LedMode mode, uint32_t durationMs){
  if (!ledOverride) currentLedMode = mode;
  if (durationMs > 0) {
    s_highPriorityUntilMs = millis() + durationMs;
    if (mode == LED_GOOD) s_connectedConfirmPending = true;
  }
}

void ledOnBoot(){
  s_installFinished = false;
  s_userActivitySeen = false;
  s_silentDeadlineMs = millis() + SILENT_TIMEOUT_MS;
  s_highPriorityUntilMs = 0;
  s_connectedConfirmPending = false;
  if (!ledOverride) currentLedMode = LED_BOOT;
}

void ledOnProvisioningStart(){
  s_userActivitySeen = true;
  s_silentDeadlineMs = 0;
  if (!ledOverride) currentLedMode = LED_PAIRING;
}

void ledOnProvisioningError(){
  if (!ledOverride) currentLedMode = LED_BAD;
}

void ledOnConnectedOk(){
  uint32_t now = millis();
  if (s_highPriorityUntilMs != 0 && now < s_highPriorityUntilMs) return;
  enterHighPriority(LED_GOOD, CONNECTED_CONFIRM_MS);
}

void updateLedState(LedMode mode){
  if (!ledOverride) currentLedMode = mode;
}

// NEW : à appeler quand tu viens de publier les données capteur
void ledNotifyPublish(){
  if (s_ledMuted) return;
  if (isHighPriorityActive()) return;
  // simple flag : la tâche LED déclenche l’anim en temps réel
  s_pulseRequested = true;
}

// ------------------------------------------------------------------
// Helpers internes
// ------------------------------------------------------------------

// true si on est dans la fenêtre nuit 22:30–07:30 (heure locale) ; si heure inconnue → true (safe sommeil).
static bool isNightNow(){
  if (!timeIsSaneHard()) return true;
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  const int minute = tm.tm_hour * 60 + tm.tm_min;
  const int nightStart = NIGHT_START_HOUR * 60 + NIGHT_START_MIN;  // 22:30
  const int nightEnd   = NIGHT_END_HOUR * 60 + NIGHT_END_MIN;      // 07:30
  return (minute >= nightStart) || (minute < nightEnd);
}

// Courbe douce 0→1→0 pour la pulsation (sinus ease-in-out)
static float pulseEnvelope(float t){
  // t normalisé dans [0,1]
  if (t <= 0.0f) return 0.0f;
  if (t >= 1.0f) return 0.0f;

  // 0 → 1 → 0 avec un sinus complet (2π)
  // t=0   → 0
  // t=0.5 → 1
  // t=1   → 0
  return 0.5f - 0.5f * cosf(2.0f * PI * t);
}

// Mini gamma approximatif pour éviter les “marches” visibles
static uint8_t applyGamma(uint8_t v){
  // approx gamma 2.0 : v' = (v/255)^2 * 255
  float f = (float)v / 255.0f;
  f = f * f;
  int out = (int)(f * 255.0f + 0.5f);
  if (out < 0) out = 0;
  if (out > 255) out = 255;
  return (uint8_t)out;
}
void ledSetAirQualityScore(float score01){
  if (score01 < 0.0f) score01 = 0.0f;
  if (score01 > 1.0f) score01 = 1.0f;
  s_airScore01 = score01;
  if (isHighPriorityActive()) return;
  if (!ledOverride) currentLedMode = LED_GOOD;
}

// HSV → RGB (h,s,v ∈ [0,1])
static void hsvToRgb(float h, float s, float v, uint8_t &r, uint8_t &g, uint8_t &b){
  if (s <= 0.0f){
    uint8_t val = (uint8_t)(v * 255.0f + 0.5f);
    r = g = b = val;
    return;
  }

  h = fmodf(h, 1.0f); if (h < 0.0f) h += 1.0f;
  float hf = h * 6.0f;
  int   i  = (int)hf;
  float f  = hf - (float)i;

  float p = v * (1.0f - s);
  float q = v * (1.0f - s * f);
  float t = v * (1.0f - s * (1.0f - f));

  float rf, gf, bf;
  switch (i){
    default:
    case 0: rf = v; gf = t; bf = p; break;
    case 1: rf = q; gf = v; bf = p; break;
    case 2: rf = p; gf = v; bf = t; break;
    case 3: rf = p; gf = q; bf = v; break;
    case 4: rf = t; gf = p; bf = v; break;
    case 5: rf = v; gf = p; bf = q; break;
  }

  r = (uint8_t)(rf * 255.0f + 0.5f);
  g = (uint8_t)(gf * 255.0f + 0.5f);
  b = (uint8_t)(bf * 255.0f + 0.5f);
}

// Donne la couleur de base (sans anim) pour chaque mode
static void baseColorForMode(LedMode mode, uint8_t &r, uint8_t &g, uint8_t &b){
  switch (mode){
    case LED_BOOT:      // bleu puissant (respiration)
      r = 30;  g = 100; b = 255;  break;

    case LED_PAIRING:   // jaune 1 Hz (setup)
      r = 220; g = 180; b = 0;    break;

    case LED_GOOD: {
      // Ici : "GOOD" = affichage continu de la qualité d’air
      float s = s_airScore01;                // 0 = vert, 1 = rouge
      if (s < 0.0f) s = 0.0f;
      if (s > 1.0f) s = 1.0f;

      // Dégradé vert (#00C850) → jaune (#FFD000) → rouge (#E62828)
      // On le fait en HSV: vert ≈ 0.33, jaune ≈ 0.16, rouge ≈ 0.0
      float h;
      if (s < 0.5f) {
        // 0.0 → 0.5 : vert → jaune
        float t = s / 0.5f;          // 0 → 1
        h = 0.33f + (0.16f - 0.33f) * t;
      } else {
        // 0.5 → 1.0 : jaune → rouge
        float t = (s - 0.5f) / 0.5f; // 0 → 1
        h = 0.16f + (0.00f - 0.16f) * t;
      }
      float sat = 0.95f;
      float val = 1.00f;
      hsvToRgb(h, sat, val, r, g, b);
      break;
    }

    case LED_MODERATE:  // si tu l’utilises encore ailleurs : ambré
      r = 220; g = 140; b = 20;   break;

    case LED_BAD:       // rouge "classe"
      r = 230; g = 40;  b = 40;   break;

    case LED_UPDATING:  // cyan
      r = 0;   g = 180; b = 200;  break;
    case LED_OFF:
    default:
      r = 0;   g = 0;   b = 0;    break;
  }
}


// ------------------------------------------------------------------
// Tâche LED : boucle d’animation
// ------------------------------------------------------------------
static void ledTask(void *){
  uint32_t lastMs = millis();
  static uint32_t lastNightCheckMs = 0;
  static bool nightMode = true;   // défaut safe sommeil
  static uint8_t currentBrightnessCap = BRIGHTNESS_NIGHT;

  for(;;){
    if (s_ledMuted){
      vTaskDelay(pdMS_TO_TICKS(40));
      continue;
    }

    uint32_t now = millis();
    uint32_t dt  = now - lastMs;
    if (dt > 50) dt = 50;        // clamp pour éviter gros sauts si pause
    lastMs = now;

    if (s_highPriorityUntilMs != 0 && now >= s_highPriorityUntilMs) {
      s_highPriorityUntilMs = 0;
      if (s_connectedConfirmPending) {
        s_installFinished = true;
        s_connectedConfirmPending = false;
        if (!ledOverride) currentLedMode = LED_GOOD;
      } else {
        if (!ledOverride) currentLedMode = LED_OFF;
      }
    }
    if (!s_installFinished && !s_userActivitySeen && s_silentDeadlineMs != 0 && now > s_silentDeadlineMs) {
      s_silentDeadlineMs = 0;
      if (!ledOverride) currentLedMode = LED_OFF;
    }

    // 1) Gestion du temps d’anim
    const float idleSpeed = 2.0f * PI / 2600.0f;   // période ~2.6s
    s_idlePhase += idleSpeed * (float)dt;
    if (s_idlePhase > 2.0f * PI) s_idlePhase -= 2.0f * PI;

    // Phase de teinte BLE (rotation lente)
    const float pairSpeed = 2.0f * PI / 4500.0f;   // ~4.5s pour un cycle
    s_pairPhase += pairSpeed * (float)dt;
    if (s_pairPhase > 2.0f * PI) s_pairPhase -= 2.0f * PI;

    const float bootSpeed = 2.0f * PI / BOOT_BREATHE_PERIOD_MS;
    s_bootPhase += bootSpeed * (float)dt;
    if (s_bootPhase > 2.0f * PI) s_bootPhase -= 2.0f * PI;

    // 2) Gestion de la pulse
    if (s_pulseRequested){
      s_pulseRequested = false;
      s_pulseT = 0.0f;          // démarre une nouvelle pulsation
    } else if (s_pulseT < 1.0f){
      s_pulseT += (float)dt / (float)PULSE_DURATION_MS;
      if (s_pulseT > 1.0f) s_pulseT = 1.0f;
    }

    // Recalcul night mode ~1 s (ou au premier passage) ; prise en compte override MQTT
    if (lastNightCheckMs == 0 || (now - lastNightCheckMs) >= 1000) {
      lastNightCheckMs = now;
      nightMode = (s_nightModeOverride == 0) ? isNightNow() : (s_nightModeOverride == 1);
      s_nightModeActive = nightMode;
      currentBrightnessCap = nightMode ? BRIGHTNESS_NIGHT : BRIGHTNESS_DAY;
    }

    // 3) Couleur de base selon le mode
    uint8_t baseR, baseG, baseB;
    LedMode mode = currentLedMode;
    baseColorForMode(mode, baseR, baseG, baseB);
    bool connectedConfirmActive = (s_highPriorityUntilMs != 0 && now < s_highPriorityUntilMs && mode == LED_GOOD);
    if (connectedConfirmActive) {
      baseR = 0; baseG = 200; baseB = 0;
    }

    // === A) Intensité "respiration" de base ========================
    float baseFactor = 1.0f;

    if (mode == LED_GOOD || mode == LED_MODERATE || mode == LED_BAD){
      // AVANT : ~0.55 → 0.65 (assez lumineux)
      // MAINTENANT : ~0.25 → 0.40 (beaucoup plus soft)
      baseFactor = 0.25f + 0.15f * (0.5f * (1.0f - cosf(s_idlePhase)));
      // min ≈ 0.25  /  max ≈ 0.40
    } else if (mode == LED_UPDATING){
      // Avant plus fort, ici on le rend aussi plus discret
      baseFactor = 0.30f + 0.20f * (0.5f * (1.0f - cosf(s_idlePhase)));
      // ~0.30 → 0.50
    } else if (mode == LED_PAIRING){
      float blink1Hz = (float)(now % 1000) / 1000.0f;
      baseFactor = (blink1Hz < 0.5f) ? 0.45f : 0.12f;
    } else if (mode == LED_BOOT){
      baseFactor = 0.55f + 0.40f * (0.5f * (1.0f - cosf(s_bootPhase)));
    } else { // LED_OFF ou autres
      baseFactor = 0.0f;
    }

    if (connectedConfirmActive) baseFactor = 0.65f;

    if (mode == LED_BAD && baseFactor > 0.0f){
      float blink4Hz = (float)(now % 250) / 250.0f;
      baseFactor *= (blink4Hz < 0.5f) ? 0.85f : 0.25f;
    }

    bool highPriority = (s_highPriorityUntilMs != 0 && now < s_highPriorityUntilMs)
      || (!s_installFinished && (mode == LED_BOOT || mode == LED_PAIRING || mode == LED_BAD || mode == LED_UPDATING));

    // === B) Pulse : bump AU-DESSUS de la respiration (désactivée la nuit) ==============
    // En erreur (LED_BAD) on n'applique pas la pulse qualité d'air → clignotement rouge uniquement
    float factor = baseFactor;

    if (!highPriority && (mode == LED_GOOD || mode == LED_MODERATE) && !nightMode){
      float env = 0.0f;
      if (s_pulseT < 1.0f){
        env = pulseEnvelope(s_pulseT);   // 0 → 1 → 0
      }

      if (env > 0.0f){
        const float pulsePeak = 1.0f;    // max avant gamma
        factor = baseFactor + (pulsePeak - baseFactor) * env;
      }
    }

    // (Option) : si tu veux aussi une petite accentuation de luminosité
    // pendant provisioning (quand tu envoies les creds, etc.), tu pourras
    // plus tard déclencher un petit “env” spécifique ici pour LED_PAIRING.

    // Clamp sécurité
    if (factor > 1.0f) factor = 1.0f;
    if (factor < 0.0f) factor = 0.0f;

    // 7) Calcul final des canaux avec "gamma" + cap luminosité day/night
    uint8_t outR = applyGamma((uint8_t)((float)baseR * factor));
    uint8_t outG = applyGamma((uint8_t)((float)baseG * factor));
    uint8_t outB = applyGamma((uint8_t)((float)baseB * factor));

    led.setBrightness(currentBrightnessCap);
    led.setPixelColor(0, led.Color(outR, outG, outB));
    led.show();

    vTaskDelay(pdMS_TO_TICKS(16));
  }
}

// Démarrage tâche
void ledTaskStart(){ 
  xTaskCreatePinnedToCore(ledTask, "LED", 4096, nullptr, 1, &s_ledTask, 1);
}
