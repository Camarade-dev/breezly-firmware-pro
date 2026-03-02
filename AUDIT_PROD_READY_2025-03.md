# Audit production-ready — Breezly ESP32-WROOM-32E

**Date:** 2 mars 2025  
**Objectif:** Êtes-vous prêt à flasher le banc de production et envoyer les appareils aux clients sans regrets ?

---

## Verdict court

| Statut | Résumé |
|--------|--------|
| **Presque prêt** | La base est solide (secrets, OTA, backoff, factory, EOL). **Une correction critique** (niveau de logs prod) et quelques points mineurs à traiter avant de flasher en confiance. |

---

## 1. Ce qui est prêt (OK)

### 1.1 Sécurité & secrets
- **Secrets hors repo** : `secrets.ini` (gitignore), `devkey.h` et `mqtt_secrets.h` générés par pre-build (gitignore). Aucune valeur en clair dans `platformio.ini`.
- **Build refusé si secrets manquants** : `pre_build_devkey.py` et `pre_build_mqtt_secrets.py` font `sys.exit(1)` si device key ou MQTT user/pass absents.
- **Post-upload** : `post_upload_register.py` exige `custom_factory_token` et `custom_device_key_b64` (ou variables d’env), sinon `RuntimeError`.

### 1.2 OTA & robustesse
- **TLS OTA** : pas de `setInsecure()` dans `ota.cpp` ; CA vérifiée pour manifeste et binaire.
- **Rollback** : après 3 boots en échec sur partition pending, rollback automatique.
- **Backoff** : exponentiel + jitter pour Wi‑Fi et MQTT (paramètres dans `app_config.h`).
- **État OTA** : unifié via `otaIsInProgress()` partout (plus de doublon `otaInProgress`).

### 1.3 Boot & télémétrie
- **Reset reason** : loggé au boot dans `main.cpp` (LOGI) et envoyé dans le message MQTT `FW_BOOT` (`mqtt_bus.cpp`).
- **Watchdog** : Task WDT 120 s, feed dans la loop et dans les tâches OTA.

### 1.4 Capteurs & payload
- **Sanity checks** : `sensorSanityCheck()` (AQI 1–5, TVOC/eCO2 bornés) ; `sanity_ok` et `sanity_fail` dans le payload.
- **I2C** : `I2C_BUS_TIMEOUT_MS` et `I2C_BUS_RESET_AFTER_FAILURES` définis dans `app_config.h`.

### 1.5 Factory & industrialisation
- **Procédure** : `FACTORY_E2E_CHECKLIST.md` à jour (flash_fleet, EOL, recovery).
- **flash_fleet.py** : build une fois, upload parallèle, journal EOL auto (`docs/EOL_LOG.csv`), variant STD/PREMIUM, opérateur.
- **EOL** : template `docs/FACTORY_EOL_LOG_TEMPLATE.csv` présent ; `EOL_LOG.csv` dans `.gitignore`.

### 1.6 Contrôle & conformité
- **Prod** : `CTRL_REQUIRE_SIG=1`, `CTRL_ALLOW_UNSIGNED=0`, `CTRL_FACTORY_RESET_ENABLED=0` dans `app_config.h`.
- **Partitions** : `partitions.csv` avec app0, app1, otadata, NVS, spiffs.

---

## 2. Correction critique avant flash prod

### 2.1 Niveau de logs en prod (à corriger)

**Problème** : En prod, `platformio.ini` impose `BREEZLY_LOG_LEVEL=4` (DEBUG). Les macros `LOGD()` sont donc actives et peuvent logger :
- SSID Wi‑Fi (`main.cpp` L318),
- `sensorId` / `userId` (`main.cpp` L322),
- longueur des payloads, etc.

En usine ou chez le client, toute personne avec un accès Serial (USB) peut voir ces infos.

**Action** : En environnement **esp32-wroom-32e-prod**, passer à **WARN (2)** ou **INFO (3)** pour ne pas exposer de données sensibles.

```ini
; Dans [env:esp32-wroom-32e-prod], remplacer :
build_flags =
  ${env.build_flags}
  -DBREEZLY_PROD
  -DBREEZLY_LOG_LEVEL=2
```

- `2` = WARN + ERROR uniquement (recommandé pour prod).
- `3` = INFO en plus (utile si vous voulez garder les messages de boot / reset_reason sans détail SSID/sensorId).

---

## 3. Points mineurs (recommandés)

### 3.1 setInsecure() dans le fallback SNTP

**Fichier** : `src/net/sntp_utils.cpp` L57.  
`syncTimeFromHttpDate()` utilise `c.setInsecure()` pour lire l’en-tête Date sur `google.com` (fallback si NTP bloqué).

- **Risque** : théorique (MITM sur l’horloge), impact limité (pas de firmware, pas de credentials).
- **Recommandation** : à moyen terme utiliser `setCACert()` avec un bundle CA pour cette connexion ; en attendant, acceptable pour un premier déploiement si documenté.

### 3.2 Serial directs (hors macros LOGx)

Plusieurs `Serial.printf` / `Serial.println` ne passent pas par `LOGE/LOGW/LOGI/LOGD` et sont donc toujours actifs, par exemple :
- `sensors.cpp` : init AHT21/ENS160, trame PMS,
- `provisioning.cpp` : debug BLE, chunks, watchdog,
- `wifi_enterprise.cpp` : raison déconnexion EAP.

En prod avec `BREEZLY_LOG_LEVEL=2`, les **LOGD** sont désactivés, mais ces **Serial** restent. En usine (câble USB) c’est souvent utile ; chez le client final, idéalement les conditionner avec `#if defined(BREEZLY_DEV)` ou un niveau log dédié. Non bloquant pour un premier lot si vous acceptez un peu de bruit Serial en debug physique.

### 3.3 Chemin PlatformIO dans flash_fleet.py

**Fichier** : `scripts/flash_fleet.py` L19.  
`DEFAULT_PIO_EXE` pointe vers un chemin Windows en dur. Sur une autre machine ou Linux, il faut passer `--pio-exe` ou adapter le défaut (ex. `shutil.which("pio")`). À documenter dans la checklist ou à rendre détecté automatiquement.

---

## 4. Checklist « sans regrets » avant flash banc prod

Cocher avant de lancer le flash de production :

- [ ] **secrets.ini** présent et rempli (device key, factory token, MQTT user/pass). Aucune valeur vide.
- [ ] **BREEZLY_LOG_LEVEL** en prod = 2 (ou 3), pas 4 (voir §2.1).
- [ ] Build prod OK : `pio run -e esp32-wroom-32e-prod` sans erreur.
- [ ] Version firmware : `CURRENT_FIRMWARE_VERSION` dans `app_config.h` incrémentée et cohérente avec la release.
- [ ] **flash_fleet** : si usage industriel, `--variant STD` (ou PREMIUM) et `--operator` renseignés ; `--env esp32-wroom-32e-prod`.
- [ ] Réseau : accès à l’API backend (post_upload_register) depuis la machine qui flashe.
- [ ] Un device de test flashé et vérifié : boot, BLE `PROV_*`, enregistrement API, (optionnel) WiFi + MQTT + une trame capteur.

---

## 5. Synthèse

| Catégorie              | Statut | Commentaire |
|------------------------|--------|-------------|
| Secrets & identité     | OK     | Hors repo, build refusé si manquants. |
| OTA & rollback         | OK     | TLS, rollback 3 boots. |
| Backoff Wi‑Fi / MQTT   | OK     | Implémenté et configuré. |
| Logs prod              | À faire | Passer prod en **BREEZLY_LOG_LEVEL=2** (ou 3). |
| Factory / EOL           | OK     | Procédure + flash_fleet + journal EOL. |
| SNTP fallback          | Mineur | setInsecure documenté ; renforcer plus tard. |
| Serial directs         | Mineur | Optionnel : conditionner en dev pour prod propre. |

**Conclusion** : Après correction du niveau de logs prod (§2.1), vous pouvez flasher votre banc de production et envoyer les appareils aux clients en limitant les regrets. Les points mineurs peuvent être traités dans une prochaine itération (sécurité SNTP, nettoyage Serial, chemin pio dans flash_fleet).
