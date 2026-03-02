# Audit : Gestion du sleep du PCB et de ses composants

**Date :** 2 mars 2025  
**Périmètre :** Light sleep, deep sleep, modem sleep, et gestion des périphériques (PMS, LED, WiFi, BLE).

---

## 1. Synthèse exécutive

| Niveau de sleep        | Utilisé ? | Où / Comment |
|------------------------|-----------|----------------|
| **Modem sleep (WiFi)** | ✅ Oui    | `enterModemSleep(true)` après publish, désactivé pendant OTA/BLE |
| **Light sleep (CPU)** | ✅ Partiel | `enableCpuPM()` → tickless idle + light sleep auto ; **pas** d’`esp_light_sleep_start()` explicite |
| **Light nap (code)**   | ⚠️ Nom trompeur | `lightNapMs()` = simple `vTaskDelay` en boucle, **aucun** light sleep réel |
| **Deep sleep**         | 🔶 Optionnel (désactivé) | `USE_DEEP_SLEEP=0` ; si activé : timer wakeup 2 min, WiFi off, PMS sleep, **LED non éteintes** |

**Composants concernés :** PMS5003 (pin SET), LED (GPIO 13), WiFi, BLE, CPU/ESP32.

---

## 2. Détail par mécanisme

### 2.1 Modem sleep (WiFi PS)

- **Fichier :** `src/power/sleep.h` → `enterModemSleep(bool enable)`  
- **API :** `esp_wifi_set_ps(enable ? WIFI_PS_MAX_MODEM : WIFI_PS_NONE)`  
- **Comportement :** Réduit la consommation du modem WiFi quand il n’y a pas de trafic.  
- **Appel :** Dans `main.cpp` après un publish réussi :  
  `enterModemSleep(true)` → `lightNapMs(INTERACTIVE_WINDOW_MS)` (30 s) → `enterModemSleep(false)`.
- **Sécurités :** Pas de modem sleep pendant `otaIsInProgress()` ni quand `bleInited` est vrai (provisioning).

**Verdict :** ✅ Correct et bien conditionné.

---

### 2.2 Light sleep (CPU / ESP-IDF)

- **Fichier :** `src/power/cpu_pm.h` → `enableCpuPM()`  
- **Config :**  
  - `light_sleep_enable = true` → le système peut entrer en light sleep automatiquement (tickless idle).  
  - Fréquence CPU : 80–240 MHz.  
- **Appel :** Une fois dans `main.cpp` → `setup()` (ligne ~282).

**Limitations :**

- Aucun appel explicite à `esp_light_sleep_start()` n’existe dans le projet.  
- La fonction `lightNapMs()` (voir ci‑dessous) ne fait **pas** de light sleep : le nom prête à confusion.

**Verdict :** ✅ Light sleep automatique (idle) activé. ⚠️ Pas de fenêtre de light sleep explicite pour les 30 s après publish.

---

### 2.3 « Light nap » (`lightNapMs`)

- **Fichier :** `src/power/sleep.h`  
- **Implémentation actuelle :**  
  ```cpp
  uint32_t end = millis() + ms;
  while ((int32_t)(end - millis()) > 0){
    vTaskDelay(50/portTICK_PERIOD_MS);
  }
  ```  
  = boucle de `vTaskDelay(50 ms)` jusqu’à expiration. **Aucun** arrêt du CPU, aucun `esp_light_sleep_*`.  
- **Usage :** Fenêtre « interactive » de 30 s après publish (`INTERACTIVE_WINDOW_MS`), pendant laquelle le modem sleep est actif mais le CPU reste actif.

**Verdict :** ⚠️ Nom trompeur ; possibilité d’économiser du courant en remplaçant par un vrai light sleep (timer wakeup) si l’architecture le permet (pas de tâches critiques à 50 ms près pendant cette fenêtre).

---

### 2.4 Deep sleep

- **Fichier :** `src/power/sleep.h` → `deepSleepForMs(uint64_t ms)`  
- **Config :** `USE_DEEP_SLEEP` dans `src/power/power_config.h` → **défini à 0** (désactivé).  
- **Si activé :**  
  - `WiFi.disconnect(true, true)`  
  - Commentaire « LEDs off » mais **aucune ligne de code** pour éteindre la LED.  
  - `pmsSleep()` appelé dans `main.cpp` juste avant `deepSleepForMs(120000)`.  
  - `esp_sleep_enable_timer_wakeup(ms * 1000)` + `esp_deep_sleep_start()`.  
- **Condition d’entrée (main.cpp) :**  
  `!otaIsInProgress() && !g_factoryResetPending && mqtt_is_connected()` puis `pmsSleep()` et `deepSleepForMs(120000)`.

**Manques si on active le deep sleep :**

1. **LED :** Non éteintes avant `esp_deep_sleep_start()` → consommation et état visuel indéfini.  
2. **Wake sources :** Uniquement le timer ; pas de wake sur GPIO (bouton, etc.) ni autre source documentée.  
3. **Reconnexion après réveil :** Le code actuel suppose un boot « normal » ; après deep sleep, tout repart de `setup()` — à confirmer que WiFi/MQTT et capteurs se réinitialisent correctement (probablement oui, mais à valider).

**Verdict :** 🔶 Deep sleep désactivé ; si activé, il faut au minimum ajouter l’extinction explicite de la LED avant `esp_deep_sleep_start()`.

---

## 3. Composants du PCB et leur gestion du sleep

### 3.1 PMS5003 (particules)

- **Pins :** SET (GPIO 15), RX/TX (16/17).  
- **Logique :**  
  - `pmsWake()`  → SET = HIGH (actif).  
  - `pmsSleep()` → SET = LOW (sleep du capteur).  
- **Où :**  
  - `pmsInitPins(15)` en setup capteurs ; état initial = sleep si `!PMS_ALWAYS_ON`.  
  - Avant chaque mesure : `pmsWake()` → warmup → burst → `pmsSleep()` (si pas `pmsAlwaysOn`).  
  - Avant deep sleep (si activé) : `pmsSleep()` dans `main.cpp`.  
- **Config :** `PMS_ALWAYS_ON` = false dans `app_config.h` → le PMS est bien mis en sleep entre les mesures.

**Verdict :** ✅ Gestion cohérente ; sleep du PMS respecté avant deep sleep.

---

### 3.2 LED (GPIO 13)

- **Comportement :** Pas de mise en sleep spécifique « LED off » dans les modules de power.  
- En deep sleep (si activé), le commentaire prévoit « LEDs off » mais **aucun appel** du type `led.off()` / `led.clear()` / `led.setBrightness(0)` dans `sleep.h` ou avant `deepSleepForMs()`.

**Verdict :** ⚠️ À corriger si `USE_DEEP_SLEEP` est mis à 1 : éteindre la LED avant `esp_deep_sleep_start()`.

---

### 3.3 WiFi

- Modem sleep géré par `enterModemSleep()`.  
- Deep sleep : `WiFi.disconnect(true, true)` avant `esp_deep_sleep_start()`.

**Verdict :** ✅ OK.

---

### 3.4 BLE

- Pas de mise en sleep explicite du BLE dans les fichiers de power.  
- `enterModemSleep()` ne s’active pas tant que `bleInited` est vrai, ce qui évite d’endormir le modem pendant le provisioning.

**Verdict :** ✅ Cohérent avec le flux (provisioning puis WiFi).

---

### 3.5 I2C (AHT21, ENS160)

- Aucune séquence spécifique « put sensors in low power » ou coupure I2C avant sleep.  
- En deep sleep, l’ESP32 coupe l’alimentation logique du bus selon le schéma ; les capteurs n’ont pas d’ordre logiciel de sleep dans le code audité.

**Verdict :** ℹ️ Acceptable pour l’instant (deep sleep désactivé) ; si on vise une conso minimale en deep sleep, vérifier la doc des capteurs (modes low power / power-down) et le câblage (pull-ups, alimentation).

---

## 4. Flux résumé

```
Boot
  → enableCpuPM() (light_sleep auto + 80–240 MHz)

Loop principal
  → Si publish capteurs OK :
       enterModemSleep(true)
       lightNapMs(30_000)   // 30 s, CPU actif !
       enterModemSleep(false)
  → Si USE_DEEP_SLEEP && mqtt connecté :
       pmsSleep()
       deepSleepForMs(120000)  // LEDs non éteintes !
```

---

## 5. Recommandations

| Priorité | Action |
|----------|--------|
| **Haute** | Si activation de `USE_DEEP_SLEEP` : **éteindre la LED** (brightness 0 ou clear) avant `esp_deep_sleep_start()`. |
| **Moyenne** | Renommer `lightNapMs()` (ex. `interactiveWindowDelayMs()`) ou implémenter un vrai light sleep avec `esp_light_sleep_start()` + timer wakeup pour la fenêtre post‑publish. |
| **Basse** | Documenter les wake sources du deep sleep (timer seul aujourd’hui) et, si besoin, ajouter un wake sur GPIO (bouton). |
| **Basse** | Vérifier après un réveil de deep sleep que WiFi, MQTT, PMS et LED se réinitialisent correctement (tests E2E). |

---

## 6. Fichiers concernés

| Fichier | Rôle sleep |
|---------|------------|
| `src/power/sleep.h` | `enterModemSleep`, `lightNapMs`, `deepSleepForMs` |
| `src/power/cpu_pm.h` | `enableCpuPM` (light sleep auto) |
| `src/power/power_config.h` | `USE_DEEP_SLEEP`, périodes PMS, `INTERACTIVE_WINDOW_MS` |
| `src/main.cpp` | Appels à `enableCpuPM`, `enterModemSleep`, `lightNapMs`, `pmsSleep`, `deepSleepForMs` |
| `src/sensors/sensors.cpp` | `pmsWake` / `pmsSleep`, séquence wake → mesure → sleep |
| `src/app_config.h` | `PMS_ALWAYS_ON` |

---

*Audit réalisé par analyse statique du dépôt ; tests de consommation et réveil deep sleep à valider en bench.*
