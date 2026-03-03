# FIRMWARE PRODUCTION READINESS AUDIT

**Cible:** ESP32-WROOM-32E (Breezly)  
**Date audit:** 2026-02 — **Dernière vérification alignée code/doc:** 2026-02  
**Framework:** PlatformIO + Arduino (espressif32 53.03.11)

---

# 0. Executive Summary

- **Statut global:** **Almost ready** — Fonctionnel mais plusieurs points bloquants sécurité et un bug structurel corrigé (voir §1).
- **5 risques majeurs (historique):** (1) Secrets en dur — mitigé (secrets.ini); (2) OTA setInsecure — supprimé; (3) Backoff Wi‑Fi/MQTT — **implémenté** (core/backoff, app_config.h); (4) Variable `otaInProgress` dupliquée — unifier avec `otaIsInProgress()`; (5) Bloc sensors+MQTT après OTA — corrigé dans loop().
- **Actions restantes avant prod:** Unifier état OTA (`otaIsInProgress()` partout); checklist factory + EOL; logs niveau.

---

# 1. Architecture réelle du firmware

## 1.1 Modules / fichiers (rôle)


| Fichier                                   | Rôle                                                                                                                                                                                           |
| ----------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/main.cpp`                            | Point d’entrée: `setup()` (init, prefs, BLE, WiFi, pas de MQTT/sensors au boot), `loop()` (OTA boot/tick, provisioning, publish capteurs, démarrage sensors+MQTT après fenêtre OTA, WDT feed). |
| `src/core/globals.cpp`, `globals.h`       | État global: WiFi, prefs, MQTT client, AHT/ENS160/PMS, LED, `otaInProgress` (doublon avec `ota.cpp`).                                                                                          |
| `src/core/devkey_runtime.cpp`, `devkey.h` | Clé device (NVS ou `DEVICE_KEY_B64` build); `loadOrInitDevKey()` appelé en `setup()`.                                                                                                          |
| `src/ble/provisioning.cpp`                | NimBLE, GATT (credentials + status), claim HMAC, watchdog session, phases provisioning.                                                                                                        |
| `src/net/wifi_connect.cpp`                | PSK + EAP; timeout 15 s par tentative; backoff exponentiel (core/backoff), retry dans loop si creds valides.                                                                                   |
| `src/net/wifi_enterprise.cpp`             | WPA2-Enterprise PEAP/MSCHAPv2, CA Rezoleo embarqué.                                                                                                                                            |
| `src/net/mqtt_bus.cpp`                    | Tâche FreeRTOS MQTT (queue 24 msgs), TLS, backoff exponentiel (core/backoff), LWT, commandes set_wifi / forget_wifi / factory_reset / update.                                                  |
| `src/net/sntp_utils.cpp`                  | SNTP + fallback HTTP Date pour horloge TLS.                                                                                                                                                    |
| `src/ota/ota.cpp`                         | Manifest signé ECDSA P-256, rollback après 3 boots en pending, GitHub Pages + fallback backend; TLS vérifié (CA_BUNDLE_PEM, P0-2).                                                             |
| `src/sensors/sensors.cpp`                 | AHT21, ENS160, PMS (UART), fusion/lissage PM, `safeSensorRead`, `pmsSampleBlocking`.                                                                                                           |
| `src/sensors/calibration.cpp`             | NVS namespace `cal`, temp/hum, `calCompose`/`calInit`.                                                                                                                                         |
| `src/led/led_status.cpp`                  | Tâche LED (modes, score qualité d’air, pulse).                                                                                                                                                 |
| `src/power/sleep.h`, `power_config.h`     | Modem sleep, `lightNapMs`, `deepSleepForMs` (si `USE_DEEP_SLEEP`).                                                                                                                             |
| `src/app_config.h`                        | Version, URLs manifest, LED, OTA interval, **paramètres backoff** (Wi‑Fi: min/max/factor/jitter/auth_fail_min; MQTT: min/max/factor/jitter).                                                   |
| `src/core/backoff.h`, `backoff.cpp`       | Module backoff exponentiel générique (reset, onFailure, shouldAttempt, getState); policies Wi‑Fi (auth_fail 30s) et MQTT; simulation sous `BACKOFF_SIM_TEST`.                                  |


## 1.2 Flux principaux

1. **Boot:** `setup()` → delay 500 ms → (optionnel) `resetWifiOnEveryFlash()` si `FACTORY_RESET_ON_FLASH` → `enableCpuPM()` → `otaOnBootValidate()` (rollback si 3 boots failed) → `twdtInitOnce()` (120 s) → LED, prefs (WiFi/sensorId/userId/EAP), `loadOrInitDevKey()` → `setupBLE(needProv)` → pas d’init sensors/PMS ici → si creds présents `connectToWiFi()` → pas de démarrage MQTT dans setup.
2. **Provisioning:** BLE advertising si pas de creds valides; app envoie JSON (op: provision/claim_challenge/erase/factory_reset/etc.); credentials en NVS; `needToConnectWiFi = true` → `loop()` appelle `connectToWiFi()`.
3. **Run:** Une fois WiFi + fenêtre OTA terminée, `loop()` pose `s_mqttStarted` et lance `sensorsInit()`, `calInit()`, `calCompose()`, `pmsTaskStart`, `mqtt_bus_start_task()`, `mqtt_request_connect()`. Ensuite: si MQTT connecté, lecture ENS/PMS périodique, publish `capteurs/qualite_air`, score LED, modem sleep / light nap.
4. **Publication data:** Périodes depuis `power_config.h` (`ENS_READ_PERIOD_MS_DAY`, `PMS_SAMPLE_PERIOD_MS_DAY`), première mesure ENS immédiate; JSON avec temp/hum/AQI/TVOC/eCO2 et PMS (ux/atm/cf1/counts).
5. **OTA:** Au premier WiFi connecté une tâche `OTA_BOOT` fait `checkAndPerformCloudOTA()`; puis toutes les `OTA_CHECK_INTERVAL_MS` (12 h) tâche `OTA_TICK`; aussi déclenchable via MQTT `action: "update"`. Manifest signé, téléchargement .bin, SHA256, `esp_ota_set_boot_partition`, NVS `ota.pending=true`, reboot.
6. **Erreurs:** Factory reset (MQTT ou flag), forget_wifi, rollback OTA après 3 échecs de boot; WiFi disconnect → BLE re-advertising si pas encore provisioned; MQTT déco → reconnexion 8 s.

## 1.3 Points d’entrée et dépendances critiques

- **setup:** `main.cpp` → globals, prefs, devkey, BLE, WiFi (pas MQTT/sensors).
- **loop:** `main.cpp` → OTA, provisioning, publish (si MQTT connecté), démarrage sensors+MQTT une fois `g_otaBootWindowDone` et WiFi OK, WDT feed, optionnel deep sleep.
- **Critique:** MQTT et sensors ne démarrent qu’après `g_otaBootWindowDone`; si cette fenêtre ne se termine jamais (ex. pas de WiFi au boot), les capteurs et MQTT ne démarrent pas (comportement voulu pour éviter conflits OTA).

---

# 2. Checklist Production Readiness


| Catégorie                  | Item                             | Status  | Evidence                                                                                                                                             | Risk                                               | Fix proposé                                                   |
| -------------------------- | -------------------------------- | ------- | ---------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------- | ------------------------------------------------------------- |
| **Sécurité & identité**    | Device key hors firmware         | OK      | P0-1: `platformio.ini` sans valeur; `secrets.ini` (gitignore) ou env `DEVICE_KEY_B64`; `pre_build_devkey.py` lit option/env → `devkey.h` (gitignore) | —                                                  | —                                                             |
|                            | Factory token hors firmware      | OK      | P0-1: idem via `custom_factory_token` / `FACTORY_TOKEN`; `post_upload_register.py` lit option/env                                                    | —                                                  | —                                                             |
|                            | Pas de secrets MQTT en dur       | OK      | P0-1: `mqtt_bus.cpp` L31-32 utilise `MQTT_SECRET_USER`/`MQTT_SECRET_PASS`; `pre_build_mqtt_secrets.py` → `mqtt_secrets.h` (gitignore)                | —                                                  | —                                                             |
| **Provisioning & UX**      | BLE provisioning opérationnel    | OK      | `provisioning.cpp`: NimBLE, claim HMAC, phases, watchdog session                                                                                     | —                                                  | —                                                             |
|                            | Reconnexion après nouveaux creds | OK      | `needToConnectWiFi` → `connectToWiFi()` dans loop                                                                                                    | —                                                  | —                                                             |
| **Réseau**                 | Backoff exponentiel Wi‑Fi        | OK      | `core/backoff.h` + `wifi_connect.cpp`: min 1s, max 5 min, facteur 2, jitter ±10%; auth_fail min 30s; reset à la connexion stable                     | —                                                  | —                                                             |
|                            | Backoff MQTT                     | OK      | `mqtt_bus.cpp`: même module Backoff, min 2s, max 5 min; reset à session établie; suspendu si Wi‑Fi down / OTA                                        | —                                                  | —                                                             |
|                            | Offline / queue                  | PARTIAL | Queue 24 messages, `mqtt_flush(timeout)`; pas de politique "last known good" documentée                                                              | Perte de trames si déco longue                     | Conserver last payload status; doc perte acceptable           |
| **Capteurs**               | Timeouts / erreurs               | PARTIAL | `safeSensorRead` retourne false si NaN; `pmsSampleBlocking(warmup)` avec timeout implicite; pas de timeout I2C explicite                             | Bus I2C bloqué possible                            | Timeout I2C (ex. 500 ms), reset bus après N échecs            |
|                            | Valeurs aberrantes               | PARTIAL | Calibration temp/hum; PMS checksum trame; pas de plafond explicite AQI/TVOC/eCO2                                                                     | Valeurs extrêmes publiées                          | Sanity checks (min/max) avant publish                         |
| **OTA**                    | Mécanisme présent                | OK      | `ota.cpp`: manifest signé, SHA256, rollback 3 boots                                                                                                  | —                                                  | —                                                             |
|                            | Pas de setInsecure en prod       | OK      | `ota.cpp`: TLS via `setCACert(CA_BUNDLE_PEM)` pour manifest et .bin (setInsecure supprimé, P0-2)                                                     | —                                                  | —                                                             |
|                            | Rollback                         | OK      | `otaOnBootValidate()`, `esp_ota_mark_app_invalid_rollback_and_reboot` (IDF 4+)                                                                       | —                                                  | —                                                             |
| **Watchdog / crash**       | WDT actif                        | OK      | `main.cpp` L54-76: Task WDT 120 s, feed dans loop                                                                                                    | —                                                  | —                                                             |
|                            | Reset reason loggé               | MISSING | Pas d’appel `esp_reset_reason()` / télémétrie                                                                                                        | Difficile de diagnostiquer reboots                 | Logger reset reason au boot, optionnel publish status         |
| **Logging**                | Niveaux / désactivation prod     | PARTIAL | `CORE_DEBUG_LEVEL=0` (platformio.ini); `OTA_DEBUG` dans ota.cpp; beaucoup de `Serial.printf` en dur                                                  | Logs verbeux en prod si OTA_DEBUG élevé            | Macro log niveau (INFO/WARN/ERR), désactivables par build     |
|                            | Pas de console.log debug         | PARTIAL | Plusieurs `Serial.println(s)` (ex. main L412, 424) pour payload                                                                                      | Bruit et possible fuite de données                 | Remplacer par logs conditionnels niveau                       |
| **Performance**            | Heap / fragmentation             | PARTIAL | OTA utilise `heap_caps_malloc`; pas de métrique heap min exposée                                                                                     | OOM en OTA ou long run                             | Exposer heap min (NVS ou status MQTT)                         |
| **Stockage**               | NVS layout défini                | PARTIAL | `myApp` (wifi, sensorId, userId, devKey, etc.), `ota`, `cal`; pas de doc layout                                                                      | Risque d’écrasement / migration                    | Doc NVS (noms, tailles), version namespace si besoin          |
| **Conso / thermique**      | Modem sleep                      | OK      | `sleep.h` `enterModemSleep`, `lightNapMs` après publish                                                                                              | —                                                  | —                                                             |
| **Conformité commerciale** | Safe defaults                    | PARTIAL | `FACTORY_RESET_ON_FLASH=0`; pas de dev key en dur si option désactivée en prod                                                                       | —                                                  | Prod: pas de FORCE_DEVKEY_FROM_BUILD sans injection sécurisée |
|                            | Pas de secrets en dur            | OK      | secrets.ini (gitignore), mqtt_secrets.h généré par pre-build, platformio.ini sans valeurs (P0-1)                                                     | —                                                  | —                                                             |
| **Build / release**        | Reproductible                    | PARTIAL | `pre_build_flashsig.py` timestamp; `FLASH_BUILD_SIG` pour reset WiFi par flash                                                                       | Build non reproductible (sig change à chaque fois) | Option build reproductible (SOURCEDATE_EPOCH ou tag)          |
|                            | Version / artefacts              | PARTIAL | `CURRENT_FIRMWARE_VERSION` dans app_config; pas de script release / tag                                                                              | Traçabilité release faible                         | Script: tag git, binaire nommé, manifest généré               |
| **Factory**                | Flash usine documenté            | PARTIAL | `post_upload_register.py` enregistre device; pas de doc procédure                                                                                    | Erreurs en usine                                   | Doc + checklist EOL (voir §7)                                 |


---

# 3. Sécurité — audit concret

## 3.1 Secrets

- **Où (après P0-1):**  
  - Secrets uniquement dans `secrets.ini` (gitignore) ou variables d’environnement.  
  - `platformio.ini`: plus de valeurs en clair; `devkey.h` et `mqtt_secrets.h` générés par pre-build (gitignore).  
  - `mqtt_bus.cpp`: utilise `MQTT_SECRET_USER` / `MQTT_SECRET_PASS` depuis `mqtt_secrets.h`.
- **Risques:** Réduits si `secrets.ini` et fichiers générés ne sont jamais commités.
- **Sortir du firmware (fait P0-1):**  
  - MQTT: soit NVS (provisioning), soit build-time via env (ex. `MQTT_USER`, `MQTT_PASS`) lus dans un fichier généré par script (ex. `src/net/mqtt_secrets.h` généré par pre_build, gitignore).  
  - CI: variables d’environnement; pas de secrets dans les logs.

## 3.2 TLS

- **Validation cert:** MQTT utilise `s_tls.setCACert(CA_BUNDLE_PEM)` (`mqtt_bus.cpp` L406) → OK pour broker.  
- **OTA:** (P0-2) Manifest et .bin utilisent `setCACert(CA_BUNDLE_PEM)` (ca_bundle.h). Plus de `setInsecure()` — TLS vérifié pour GitHub Pages et backend.  
- **Pinning:** Non utilisé.  
- **Recommandation:** Optionnel: pinning du certificat ou host pour renforcer encore.

## 3.3 Auth backend

- Device: claim HMAC avec `g_deviceKeyB64` (NVS ou build); post_upload enregistrement avec `deviceKeyB64` + `X-Factory-Token`.  
- MQTT: user/password broker fournis par `mqtt_secrets.h` (pre-build, P0-1).

## 3.4 BLE

- GATT WRITE sans chiffrement explicite (lien BLE non sécurisé); claim par HMAC côté app/backend.  
- Surface: quelqu’un à proximité peut envoyer des WRITE; sans claim valide le device ne s’associe pas au backend. Mitigation: timeout session (watchdog), pas de données sensibles en clair dans les notifs si possible.

## 3.5 Hardening (compatibilité code actuel)

- **Secure Boot / Flash Encryption:** Non utilisés. Compatibles avec le code actuel; nécessitent une clé de chiffrement et une procédure de flash adaptée (pre-burn).  
- **JTAG disable:** Non vérifié; à activer en production (eFuse).  
- **eFuse / partitioning:** Partition OTA déjà en place; Secure Boot et flash encryption se configurent dans menuconfig / sdkconfig (Arduino garde des défauts).  
- **Recommandation:** Documenter les étapes (Secure Boot optionnel, Flash Encryption si données sensibles en NVS), tester sur une carte dédiée avant déploiement.

---

# 4. OTA — audit concret

## 4.1 Mécanisme

- Manifest JSON (version, url, size, sha256, sig ECDSA P-256); vérification de la signature; téléchargement .bin via HTTP; SHA256; `esp_ota_begin/write/end`, `esp_ota_set_boot_partition`; NVS `ota.pending=true`; reboot.  
- Rollback: au boot, si `ota.pending` et `fail` ≥ 3 → `esp_ota_mark_app_invalid_rollback_and_reboot` (IDF 4+).

## 4.2 Scénarios d’échec

- **Coupure pendant écriture:** Image incomplète; au boot `esp_ota_begin` ou validation peut échouer; rollback après 3 boots.  
- **Image corrompue:** Vérification SHA256 (ota.cpp); si mismatch, pas de `set_boot_partition` → pas de brick.  
- **Downgrade:** Pas de politique explicite (pas de comparaison "version min"); on peut ajouter `min_version` dans le manifest (déjà lu L451) pour forcer mise à jour.  
- **Serveur indispo:** `fetchWithRetry` (4 tentatives); fallback backend (ota.cpp L384-402); pas de retry infini.

## 4.3 Versions / canary / staged

- Comparaison semver dans `cmpSemver`; `rollout` % par bucket `sensorId` (ota.cpp L459-461); `blocked_versions` et `min_version` (L451, L464-470). Pas de canary formalisé; le rollout % suffit pour un déploiement progressif.

## 4.4 Partition table + rollback

- `partitions.csv`: app0, app1, otadata; rollback géré par IDF. Aucun changement nécessaire pour le code actuel.

## 4.5 Recovery

- Pas de "recovery mode" dédié. En cas de brick: flash manuelle via UART (esptool) avec une image connue bonne. Recommandation: documenter la procédure (port, commande esptool, fichier .bin de secours).

---

# 5. Robustesse & fiabilité terrain

## 5.1 Wi‑Fi / MQTT

- **Wi‑Fi:** `connectToWiFi()` timeout 15 s par tentative; en échec, backoff exponentiel (min 1s, max 5 min, facteur 2, jitter ±10%). Auth fail → min 30s pour éviter de marteler la box. Reset du backoff à connexion stable (IP + internet OK). Retry dans `loop()` via `wifiBackoffShouldAttempt()`; pas de retry si provisioning (pas de creds). Pendant OTA on ne lance pas de reconnect agressif.
- **MQTT:** Module `Backoff` partagé (min 2s, max 5 min); reset à session établie (premier connect OK). Si Wi‑Fi down, la tâche MQTT ne tente pas (pas de retries actifs). Pendant OTA la tâche fait `vTaskDelay(100)` sans tenter connect. Paramètres dans `app_config.h` (BACKOFF_WIFI_*, BACKOFF_MQTT_*).

## 5.2 Offline

- Queue 24 messages; `mqtt_flush(200)` après publish. Pas de buffer "last known good" ni politique de perte documentée.  
- Recommandation: garder en NVS ou RAM le dernier payload status (optionnel); documenter que les données capteur sont "best effort" en déco.

## 5.3 Capteurs

- **Timeouts:** AHT/ENS160 via Wire (I2C); pas de timeout explicite. PMS: `pmsSampleBlocking(warmup)` puis lecture si `gPms.lastMs` < 5 s.  
- **Bus I2C:** Pas de reset après erreur. Recommandation: `Wire.setTimeOut(500)` (ou équivalent); après N échecs consécutifs, `Wire.end()` / `Wire.begin()`.  
- **Sanity:** Temp/hum bornées par calibration; ajouter plafonds pour AQI/TVOC/eCO2 avant envoi (ex. AQI 1–5, TVOC/eCO2 max 65535 ou valeur métier).

## 5.4 WDT

- Task WDT 120 s; `twdtResetSafe()` dans loop et dans les tâches OTA. Les tâches OTA s’ajoutent/suppriment du WDT. Pas de feed dans la tâche MQTT longue (loop avec `vTaskDelay(10)`); 120 s suffisant si pas de blocage.

## 5.5 Reset reason

- Aucun log ni télémétrie. Recommandation: au début de `setup()`, `esp_reset_reason()` et `esp_get_free_internal_heap_size()`; les logger (Serial ou NVS); optionnel: les inclure dans un message status MQTT au premier connect.

---

# 6. Observabilité & support

## 6.1 Logs

- **Niveaux:** `CORE_DEBUG_LEVEL=0`; dans ota.cpp `OTA_DEBUG` (0/1/2). Beaucoup de `Serial.printf` sans niveau.  
- **Recommandation:** Macro du type `LOG_INFO`, `LOG_WARN`, `LOG_ERR` contrôlées par `#if LOG_LEVEL >= X` ou flag build (ex. `-DLOG_LEVEL=1` en dev, 0 en prod). Pas de `Serial.println(payload)` en prod.

## 6.2 Métriques utiles

- À exposer (NVS ou status MQTT périodique): uptime, RSSI, nombre de reconnexions Wi‑Fi/MQTT, heap libre min, reset reason, version firmware, OTA pending/fail count.  
- Implémentation: variable `heap_min` mise à jour dans loop; compteurs reconnect dans wifi_connect et mqtt_bus; message status toutes les X minutes ou au boot.

## 6.3 Diagnostic à distance

- MQTT: `breezly/devices/{sensorId}/control` (set_wifi, forget_wifi, factory_reset, update); status sur `.../status`. Pas d’endpoint "diagnostic" dédié (ex. ping, heap, reset reason); peut être ajouté dans le payload status ou une commande `diagnostic` → publish réponse.

## 6.4 Support playbook

- Vérifier WiFi (RSSI, reconnexions); MQTT (connecté, LWT); dernier status; reset reason; version firmware; si OTA en échec, vérifier manifest/URL et logs OTA. Procédure: 1) Logs Serial si accès physique; 2) Backend/cloud: dernier status et événements; 3) Si brick: flash UART + image de secours.

---

# 7. Factory & Industrialisation

## 7.1 Flash usine

- **Outils:** PlatformIO / esptool (upload_speed 921600).  
- **Scripts:** `pre_build_devkey.py` (injecte device key), `pre_build_flashsig.py` (signature build), `post_upload_register.py` (enregistrement device via API avec factory token).  
- **Vérifs:** Après flash: MAC lue par esptool, enregistrement API réussi (post_upload); optionnel: premier boot et vérification BLE name / WiFi non configuré.

## 7.2 Provisioning en production

- Device identity: `PROV_{MAC_reversed}` (post_upload_register); BLE name idem (provisioning.cpp). Pas de QR code dans le firmware; à gérer côté app/back-office (QR = SSID/claim info si besoin).

## 7.3 Calibration capteurs

- Présente: NVS `cal`, temp/hum (Af, Bf, Ag, Bg, dUser); `calInit`/`calCompose` (calibration.cpp). Pas de protocole EOL documenté (comment caler en usine, stockage étalons).

## 7.4 EOL

- Liste suggérée: alimentation OK; boot jusqu’à BLE advertising; BLE discoverable; (optionnel) lecture I2C AHT/ENS160; (optionnel) UART PMS si câblé; pas de crash 2 min. Temps estimé: 3–5 min par device si manuel; à automatiser (script Python + serial + BLE scan) si volume.

## 7.5 Numéros de série / traçabilité

- Pas de champ "serial" dédié; identifiant = sensorId (NVS) + MAC (PROV_). Recommandation: NVS `myApp.serialNumber` (ou namespace `factory`) écrit une seule fois au premier boot / en usine; lire MAC + flash date pour générer un S/N unique.

---

# 8. Tests & Qualité

## 8.1 Tests existants

- Aucun test unitaire / intégration / hardware-in-loop repéré dans le repo. Lacune majeure pour la non-régression.

## 8.2 Plan minimal avant shipping

- **Smoke:** Flash prod build → boot → BLE visible → provisioning → WiFi + MQTT → au moins une publish capteur → OTA (staging) → reboot sur nouvelle version.  
- **Régression:** Même scénario + factory reset + forget_wifi + reconnexion après coupure WiFi/MQTT.

## 8.3 Non-régression OTA

- Après OTA: vérifier version, rollback si 3 boots en échec; tester downgrade refusé si min_version implémenté.

## 8.4 Fuzz / chaos

- Plan: (1) Fuzz JSON BLE (credentials characteristic); (2) Chaos réseau: déconnexions aléatoires Wi‑Fi/MQTT, latence élevée; (3) Outils: AFL/LLVM fuzz pour JSON; script Python coupant WiFi/MQTT. Non implémenté; à planifier (effort M/L).

---

# 9. Plan d’actions priorisé

## P0 (bloquant avant shipping)


| Action                                                                       | Effort | Risque réduit             | Fichiers                                                     | Definition of done                                                                     |
| ---------------------------------------------------------------------------- | ------ | ------------------------- | ------------------------------------------------------------ | -------------------------------------------------------------------------------------- |
| Retirer tous les secrets du code versionné (MQTT, device key, factory token) | M      | Fuite secrets, conformité | platformio.ini, mqtt_bus.cpp, secrets.ini (créer), pre_build | secrets.ini gitignore; build OK via env / secrets.ini; aucun secret en clair dans repo |
| Supprimer setInsecure() OTA pour GitHub                                      | S      | MITM firmware             | ota.cpp                                                      | CA ou pinning pour l’hébergeur OTA; build + OTA testés                                 |
| Vérifier que le bloc "Start sensors + MQTT after OTA" est bien dans loop()   | S      | Device ne publie jamais   | main.cpp                                                     | Déjà corrigé; un cycle loop exécute le bloc quand conditions remplies                  |


## P1 (fortement recommandé)


| Action                                             | Effort | Risque réduit                | Fichiers                                 | Definition of done                                                    |
| -------------------------------------------------- | ------ | ---------------------------- | ---------------------------------------- | --------------------------------------------------------------------- |
| Backoff exponentiel + jitter Wi‑Fi et MQTT         | M      | Blacklist AP, charge serveur | wifi_connect.cpp, mqtt_bus.cpp, main.cpp | Délai max 5–10 min; jitter; test déco/reco                            |
| Unifier état OTA (otaIsInProgress() partout)       | S      | Comportement OTA incorrect   | main.cpp, sleep.h                        | Aucune référence à `otaInProgress` global; tout via otaIsInProgress() |
| Log reset reason + heap au boot (optionnel status) | S      | Diagnostic terrain           | main.cpp, évent. mqtt_bus (status)       | Serial + optionnel champ MQTT status                                  |
| Procédure factory + EOL documentée                 | M      | Erreurs usine                | FACTORY_E2E_CHECKLIST.md, README ou doc  | Checklist exécutable + temps estimé                                   |


## P2 (amélioration)


| Action                                                       | Effort | Risque réduit        | Fichiers                                        | Definition of done                                  |
| ------------------------------------------------------------ | ------ | -------------------- | ----------------------------------------------- | --------------------------------------------------- |
| Macro logs par niveau (INFO/WARN/ERR), désactivables en prod | M      | Bruit, fuite données | Log macro central, main, ota, mqtt_bus, sensors | Build prod sans logs payload; niveaux configurables |
| Timeout I2C + reset bus après N échecs                       | S      | Bus bloqué           | sensors.cpp                                     | Timeout 500 ms; après 3 échecs Wire end/begin       |
| Sanity checks AQI/TVOC/eCO2 avant publish                    | S      | Valeurs aberrantes   | main.cpp ou sensors                             | Plafonds définis, valeurs clampées                  |
| Build reproductible (SOURCEDATE_EPOCH / tag)                 | S      | Traçabilité binaire  | pre_build_flashsig.py, CI                       | Même tag → même binaire (hors sig si souhaité)      |
| NVS layout documenté (namespace, clés)                       | S      | Migrations futures   | Doc / annexe                                    | Table clé/usage par namespace                       |


---

# 10. Annexes

## 10.1 Configs / build flags (principaux)

- `platformio.ini`: `-Os`, `-DCORE_DEBUG_LEVEL=0`, `-DFORCE_DEVKEY_FROM_BUILD=1`, `-DHAVE_MQTT_CLIENT_CERT`, `-DFACTORY_RESET_ON_FLASH=0`; env dev: `-DBREEZLY_DEV`; prod: `-DBREEZLY_PROD`.  
- `extra_configs = secrets.ini` (à utiliser pour les secrets).  
- `board_build.embed_txtfiles`: ca_rezoleo.pem, hivemq_ca.pem.

## 10.2 Partition table

- NVS 0x5000, otadata 0x2000, app0 0x310000, app1 0x310000, spiffs 0x100000 (partitions.csv).

## 10.3 Endpoints / topics MQTT

- **Broker:** 607207c4394d44b8bad11a33e8ed591d.s1.eu.hivemq.cloud:8883 (TLS).  
- **Prefix:** `dev/` ou `prod/` selon build.  
- **Topics:** `{prefix}breezly/devices/{sensorId}/status` (LWT, status, acks), `.../control` (set_wifi, forget_wifi, factory_reset, update), `.../ota`, `.../hello`; publish boot `{prefix}capteurs/boot`; données `capteurs/qualite_air` (avec prefix si pas déjà préfixé).

## 10.4 Glossaire

- **PSK:** Pre-Shared Key (Wi‑Fi classique).  
- **EAP:** Extensible Auth Protocol (WPA2-Enterprise).  
- **LWT:** Last Will and Testament (MQTT).  
- **OTA:** Over-The-Air (mise à jour firmware).  
- **NVS:** Non-Volatile Storage (ESP32).  
- **WDT:** Watchdog Timer.  
- **PMS:** Particulate Matter Sensor (UART).  
- **ENS160:** Capteur qualité d’air (I2C).  
- **AHT:** Temp/humidity (AHT21, I2C).

