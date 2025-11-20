#include "led_status.h"
#include <Arduino.h>
#include "../core/globals.h"    // led, currentLedMode, ledOverride

// Tâche RTOS
static TaskHandle_t s_ledTask   = nullptr;
static volatile bool s_ledMuted = false;

// --- PULSE sur publish capteurs ---
static volatile bool s_pulseRequested = false;

// 0.0 → 1.0 sur la durée d’une impulsion
static float s_pulseT = 1.0f;          
static const uint32_t PULSE_DURATION_MS = 1800;  // durée totale de la pulsation

// Phase pour le "idle breathing" très léger
static float s_idlePhase = 0.0f;

// Phase dédiée au dégradé BLE pairing (rotation de teinte)
static float s_pairPhase = 0.0f;

// ------------------------------------------------------------------
// Init
// ------------------------------------------------------------------
void ledInit(uint8_t, uint8_t){
  led.begin();
  led.setBrightness(60);   // brightness global doux (tu peux ajuster 40–80)
  led.show();
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
void updateLedState(LedMode mode){
  if (!ledOverride) currentLedMode = mode;
}

// NEW : à appeler quand tu viens de publier les données capteur
void ledNotifyPublish(){
  if (s_ledMuted) return;
  // simple flag : la tâche LED déclenche l’anim en temps réel
  s_pulseRequested = true;
}

// ------------------------------------------------------------------
// Helpers internes
// ------------------------------------------------------------------

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
    case LED_BOOT:      // bleu doux
      r = 10;  g = 40;  b = 120;  break;
    case LED_PAIRING:   // on override plus bas avec un gradient dynamique
      r = 10;  g = 60;  b = 180;  break;
    case LED_GOOD:      // vert premium (pas trop agressif)
      r = 0;   g = 200; b = 80;   break;
    case LED_MODERATE:  // jaune/ambré
      r = 220; g = 140; b = 20;   break;
    case LED_BAD:       // rouge plus classe, pas 255 pur
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

  for(;;){
    if (s_ledMuted){
      vTaskDelay(pdMS_TO_TICKS(40));
      continue;
    }

    uint32_t now = millis();
    uint32_t dt  = now - lastMs;
    if (dt > 50) dt = 50;        // clamp pour éviter gros sauts si pause
    lastMs = now;

    // 1) Gestion du temps d’anim
    const float idleSpeed = 2.0f * PI / 2600.0f;   // période ~2.6s
    s_idlePhase += idleSpeed * (float)dt;
    if (s_idlePhase > 2.0f * PI) s_idlePhase -= 2.0f * PI;

    // Phase de teinte BLE (rotation lente)
    const float pairSpeed = 2.0f * PI / 4500.0f;   // ~4.5s pour un cycle
    s_pairPhase += pairSpeed * (float)dt;
    if (s_pairPhase > 2.0f * PI) s_pairPhase -= 2.0f * PI;

    // 2) Gestion de la pulse
    if (s_pulseRequested){
      s_pulseRequested = false;
      s_pulseT = 0.0f;          // démarre une nouvelle pulsation
    } else if (s_pulseT < 1.0f){
      s_pulseT += (float)dt / (float)PULSE_DURATION_MS;
      if (s_pulseT > 1.0f) s_pulseT = 1.0f;
    }

    // 3) Couleur de base selon le mode
    uint8_t baseR, baseG, baseB;
    LedMode mode = currentLedMode;
    baseColorForMode(mode, baseR, baseG, baseB);

    // >>> Spécial LED_PAIRING : dégradé bleu/cyan/violet premium
    if (mode == LED_PAIRING){
      // hNorm centré sur ~0.58 (bleu) avec un petit range
      // ça oscille grosso modo entre cyan-ish et violet-ish
      float hNorm = 0.58f + 0.10f * sinf(s_pairPhase);   // [0.48 ; 0.68]
      float sat   = 0.55f;  // saturation modérée
      float val   = 1.00f;  // value max, on gère la luminosité avec baseFactor

      hsvToRgb(hNorm, sat, val, baseR, baseG, baseB);
    }

    // === A) Intensité "respiration" de base ========================
    float baseFactor = 1.0f;

    if (mode == LED_GOOD || mode == LED_MODERATE || mode == LED_BAD){
      // respiration très discrète : ~0.55 → 0.65
      baseFactor = 0.55f + 0.10f * (0.5f * (1.0f - cosf(s_idlePhase)));
    } else if (mode == LED_UPDATING){
      baseFactor = 0.70f + 0.20f * (0.5f * (1.0f - cosf(s_idlePhase)));
    } else if (mode == LED_PAIRING){
      // pairing un peu plus “vivant”, mais toujours soft
      baseFactor = 0.60f + 0.18f * (0.5f * (1.0f - cosf(s_idlePhase)));
    } else if (mode == LED_BOOT){
      baseFactor = 0.7f;
    } else { // LED_OFF ou autres
      baseFactor = 0.0f;
    }

    // Petit jitter très léger pour BAD
    if (mode == LED_BAD && baseFactor > 0.0f){
      float noise  = (float)(now % 157) / 157.0f;
      float jitter = 0.97f + 0.03f * (noise - 0.5f);
      baseFactor  *= jitter;
    }

    // === B) Pulse : bump AU-DESSUS de la respiration ==============
    float factor = baseFactor;

    if (mode == LED_GOOD || mode == LED_MODERATE || mode == LED_BAD){
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

    // 7) Calcul final des canaux avec "gamma"
    uint8_t outR = applyGamma((uint8_t)((float)baseR * factor));
    uint8_t outG = applyGamma((uint8_t)((float)baseG * factor));
    uint8_t outB = applyGamma((uint8_t)((float)baseB * factor));

    led.setPixelColor(0, led.Color(outR, outG, outB));
    led.show();

    vTaskDelay(pdMS_TO_TICKS(16));
  }
}

// Démarrage tâche
void ledTaskStart(){ 
  xTaskCreatePinnedToCore(ledTask, "LED", 4096, nullptr, 1, &s_ledTask, 1);
}
