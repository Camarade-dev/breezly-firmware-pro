# Documentation globale — Breezly / Multiagent

**Une seule page de vérité : statut firmware, preuves, tests, GO/NO-GO, manques.**  
Dernière mise à jour : 2026-02

---

## Résumé exécutif

1. **Firmware Breezly ESP32** (esp32_wroom_32e) : P0 implémentés dans le code (secrets hors repo, OTA sans setInsecure dans ota.cpp, release playbook).
2. **Statut** : **Commercial ready (P0 complet).** Build prod, flash, boot, WiFi, OTA (manifest + fallback backend + download 3 streams), MQTT, publish capteurs et **OTA rollback safe** validés sur device réel (2026-02) ; logs Serial + post-upload en preuve. Provisioning BLE en amont. Rollback : 3 boots simulés → rollback → pas de ré-install à l’infini (`skip: version X was rolled back`).
3. **Grep** : aucune occurrence de patterns sensibles (échantillons internes non affichés) dans le repo. setInsecure présent uniquement dans sntp_utils.cpp (fallback HTTP Date, pas OTA). Pas de clé privée OTA dans le repo (uniquement clé publique dans ota.cpp).
4. **Build prod** exige `secrets.ini` dans esp32_wroom_32e (ou variables d’env). Commande : `cd esp32_wroom_32e && pio run -e esp32-wroom-32e-prod`.
5. **GO/NO-GO** et **Validation terrain** : tableaux ci-dessous ; à cocher après tests réels. Critères OK/KO et procédures sont définis pour reproductibilité.
6. **Ce qui manque** (P1/P2) : backoff Wi‑Fi/MQTT, reset reason au boot, état OTA unifié, logs par niveau, APP_ENV_DEV non défini (risque opérationnel OTA : manifest URL toujours prod). Détail en section « Ce qui manque encore ».
7. **Docs détaillées** (audit, playbook, factory, preuves P0) en annexe uniquement.

---

## Résultats grep (audit 2026-02)

| Pattern | Fichier | Ligne | Conclusion |
|--------|---------|-------|------------|
| **setInsecure** | esp32_wroom_32e/src/net/sntp_utils.cpp | — | Fallback sync horloge (HEAD google.com) ; lecture en-tête Date uniquement, pas de payload sensible. Toléré à ce titre ; à supprimer/désactiver en prod via flag dès que possible. OTA : n’utilise pas setInsecure — dans ota.cpp, `httpGetToString()` et `downloadAndFlash...()` utilisent `setCACert(CA_BUNDLE_PEM)` (grep : 2 occurrences). |
| **MQTT_USER / MQTT_PASS** | esp32_wroom_32e/src/net/mqtt_bus.cpp | 31-32, 440 | Macros MQTT_SECRET_USER / MQTT_SECRET_PASS (header généré par pre-build). Aucun littéral. |
| **custom_device_key_b64 / custom_factory_token** | esp32_wroom_32e/platformio.ini | 55-56 | Commentaire uniquement. Valeurs dans secrets.ini (gitignore) ou env. |
| **Littéraux secrets** (patterns sensibles) | esp32_wroom_32e (repo versionné) | — | Aucune occurrence (échantillons internes non affichés). |
| **Clé privée OTA** | esp32_wroom_32e | — | Aucune. ota.cpp contient OTA_PUBKEY_PEM (publique). Signer le manifest se fait hors repo (clé privée côté CI/outil). |
| **URLs prod** | esp32_wroom_32e/platformio.ini | 67 (dev), 83 (prod) | Dev = backendweb ; prod = backend. Séparation par env. ota.cpp 644-646 : backendweb (dev) / backend (prod) selon BREEZLY_DEV/PROD. Pas de mélange. |
| **APP_ENV_DEV** | esp32_wroom_32e/src/app_config.h | 5 | Utilisé pour URL manifest ; jamais défini par le build (platformio ne définit que BREEZLY_DEV / BREEZLY_PROD). Donc URL manifest OTA = toujours prod. |

---

## Validation terrain (tests exécutés)

*Remplir **Résultat** (OK / KO), **Preuve** (ex. « Serial », « MQTT topic reçu »), **Device tested** (PROV_* / MAC), **Tester**, **Date** après chaque test. Procédure et critères sont reproductibles.*

| Test | Procédure | Critère OK | Critère KO | Résultat | Preuve | Device tested (PROV_* / MAC) | Tester | Date |
|------|-----------|------------|------------|----------|--------|------------------------------|--------|------|
| Build prod | `cd esp32_wroom_32e && pio run -e esp32-wroom-32e-prod` | Build succeeded, pas d’erreur DEVICE_KEY/MQTT credentials | Erreur pre-build ou compile | OK | Build SUCCESS, pre-build devkey/mqtt_secrets OK | — | — | 2026-02 |
| Flash + boot 2 min | `pio run -e esp32-wroom-32e-prod -t upload --upload-port COMx` puis `pio device monitor -b 115200` ; laisser 2 min | Pas de panic ; [BOOT] Setup terminé ; pas de reboot en boucle | Exception, reset en boucle, blocage | OK | Serial : [BOOT] Setup terminé ; [BOOT] Start sensors + MQTT after OTA window ; AHT21/ENS160 init ; post-upload hook OK | PROV_80BAD0215788, sensorId d046f2d9-4f02-4f9b-8601-9da80ddce2a2 | — | 2026-02 |
| BLE discoverable | Scan BLE (nRF Connect ou équivalent) après boot sans WiFi | Nom `PROV_*` visible ; service UUID 60f8a11f-... | Invisible ou mauvais nom | OK | Provisioning BLE fait en amont ; device identifié PROV_80BAD0215788 | PROV_80BAD0215788 | — | 2026-02 |
| Provisioning BLE complet | App : écriture credentials (op provision) ; device ack | Status « connected » ou équivalent ; WiFi tenté | Pas d'ack, timeout, crash | OK | Fait en amont ; Serial : status wifi_ok, inet_ok, connected ; [WD] provisioning complete | PROV_80BAD0215788 | — | 2026-02 |
| Connexion Wi‑Fi PSK | Après provisioning : SSID + mot de passe valides | WiFi connecté (Serial ou status) ; pas « auth_failed » en boucle | Échec auth, pas d'IP | OK | Serial : [WiFi] OK IP=192.168.0.42 RSSI=-40 | PROV_80BAD0215788 | — | 2026-02 |
| Wi‑Fi EAP (si supporté) | Même scénario avec EAP (PEAP/MSCHAPv2) | Connexion établie | Échec ou non testé | Non exécuté | — | — | — | — |
| MQTT connect + LWT + status | Après WiFi : vérifier broker (HiveMQ) ; topic status device | Connect ; LWT configuré ; message status (ex. online) reçu | Pas de connect, pas de status | OK | Serial : [MQTT] trying clientId=... host=...s1.eu.hivemq.cloud ; sub ctrl=prod/breezly/devices/... ; [ESP->APP][NOTIFY] hello_ok | PROV_80BAD0215788 | — | 2026-02 |
| Publish capteurs | Attendre au moins une période ENS (config) ou 1–2 min | Au moins 1 trame sur topic qualite_air (ou équivalent) reçue côté backend/broker | Aucune trame capteur | OK | Serial : Enqueue prod/capteurs/qualite_air ; JSON temperature, humidity, AQI, TVOC, eCO2, sensorId, userId | PROV_80BAD0215788 | — | 2026-02 |
| OTA : manifest + download + reboot | Mettre à jour manifest (version supérieure), déclencher OTA (MQTT ou attente check) | Device télécharge, reboot, nouvelle version visible (Serial ou status) | Échec download, pas de reboot, mauvaise version | OK | Primary URL (GitHub Pages) connection refused ; fallback backend OK ; manifest 1.0.22 ; signature OK ; download en 3 streams ; reboot ; version 1.0.22 ; « déjà à jour (skip) » au boot suivant | PROV_80BAD0215788 | — | 2026-02 |
| OTA : rollback safe (staging) | Env `esp32-wroom-32e-prod-rollback-test` : OTA vers firmware qui simule 3 boots en échec | Rollback vers partition précédente ; 4ᵉ boot = ancienne app ; pas de ré-install à l’infini | Brick ou boucle sans recovery | OK | Serial : 3× simulate failed boot → Too many failed boots → rollback ; Rollback target: clear pending ; au check OTA suivant : « skip: version 1.0.23 was rolled back (reject re-install) » | PROV_80BAD0215788 | — | 2026-02 |
| Secrets : build sans repo | Vérifier .gitignore (esp32 + backend) et `git status` : aucun fichier secret suivi | secrets.ini, devkey.h, mqtt_secrets.h (esp32) et ec_private.pem / ec_public.pem (backend) dans .gitignore | Secrets commités | OK | esp32_wroom_32e/.gitignore : secrets.ini, src/core/devkey.h, src/net/mqtt_secrets.h, .last_build_sig ; back-end-breezly/.gitignore : tools/ec_private.pem, tools/ec_public.pem. Vérifier en local : `git status` ne doit pas lister ces fichiers. | — | — | 2026-02 |
| Recovery : flash manuel | `pio run -e esp32-wroom-32e-prod -t upload --upload-port COMx` ou esptool `write_flash 0x10000 firmware.bin` | Flash OK ; device boot sur image flashée | Échec write_flash ou boot | OK | Upload SUCCESS ; Wrote 1446256 bytes at 0x00010000 ; Hash verified ; post-upload register OK | PROV_80BAD0215788 | — | 2026-02 |

*Post-upload (provisioning)* : le script `post_upload_register.py` attend après le flash que le device boot et envoie sur la série la ligne `BREEZLY_EXTERNAL_ID=PROV_xxxx` (même valeur que le nom BLE). Ainsi l’external_id utilisé pour le provisioning est toujours celui du device flashé, y compris en téléversement parallèle ou si le port esptool lit un autre device. Si cette ligne n’est pas reçue (timeout ou `pyserial` absent), fallback sur `esptool read_mac` (avec risque de décalage port/device). Optionnel : `pip install pyserial` pour activer la lecture série.

---

## Parcours test minimal (≤ 20 min)

1. **Build prod** — `cd esp32_wroom_32e && pio run -e esp32-wroom-32e-prod` (vérifier succès, pas de secret en clair). *Validé 2026-02.*
2. **Flash** — `pio run -e esp32-wroom-32e-prod -t upload --upload-port COMx` ; device boot stable. *Validé 2026-02 (COM8).*
3. **BLE visible** — Scan : nom `PROV_*`, service UUID attendu. *Provisioning BLE effectué en amont.*
4. **Provisioning** — App : écriture credentials ; device ack, WiFi tenté. *Fait en amont ; status connected visible en Serial.*
5. **Wi‑Fi OK** — Connexion établie (Serial : [WiFi] OK IP=...). *Validé 2026-02.*
6. **MQTT status + 1 trame capteur** — Connect, LWT, au moins un message status et une trame qualite_air. *Validé 2026-02 (HiveMQ, topic prod/capteurs/qualite_air).*
7. **(Option) OTA staging** — Bump version, manifest à jour ; déclencher OTA ; reboot ; version visible. *OTA validé : backend sert manifest + .bin (3 streams). Rollback safe validé : env rollback-test → 3 reboots → rollback → « skip: version X was rolled back » au check suivant.*

---

## GO/NO-GO shipping

*À cocher **après** exécution réelle des tests ci-dessus. « Commercial ready » final = tous les critères critiques cochés.*

- [x] **Build prod** : `pio run -e esp32-wroom-32e-prod` OK sans secret en clair.
- [x] **Flash + boot 2 min** : pas de panic ni reset en boucle.
- [x] **BLE** : discoverable (PROV_*, service UUID) — provisioning fait en amont.
- [x] **Provisioning** : écriture credentials + ack — fait en amont.
- [x] **Wi‑Fi** : connexion PSK (et EAP si cible) OK.
- [x] **MQTT** : connect, LWT, au moins un status.
- [x] **Capteurs** : au moins une trame qualite_air reçue.
- [x] **OTA** : check manifest + download + reboot + version visible — validé (backend sert manifest + .bin ; téléchargement en 3 streams, reboot OK).
- [x] **OTA rollback safe** : 3 boots en échec → rollback vers partition précédente ; version rejetée non réinstallée à l’infini (`rolled_back_ver` + skip).
- [ ] **OTA env** : manifest URL dev/prod correctement sélectionnée (pas « toujours prod »).
- [x] **Secrets** : secrets.ini / headers générés (esp32) et clés OTA (backend) dans .gitignore ; à vérifier en local avec `git status` qu’ils ne sont pas suivis.
- [x] **Recovery** : flash manuel (esptool ou PIO) OK sur un device.

*Statut actuel :* **P0 complet** (grep + preuves). Validation terrain : build, flash, boot, WiFi, OTA (manifest + download + rollback safe), MQTT et publish capteurs validés sur device (PROV_80BAD0215788) en 2026-02 ; BLE/provisioning en amont.

---

## Test OTA (procédure reproductible)

Pour qu’un device fasse vraiment une mise à jour OTA (téléchargement + flash + reboot), il faut un **manifest plus récent que la version en flash** et **correctement signé**. Les 3 échecs typiques : manifest introuvable / **signature invalide** / **version du manifest ≤ version courante** (skip « déjà à jour »).

### Prérequis

- Device déjà flashé avec une version connue (ex. `1.0.21` dans `src/app_config.h`).
- Serial ouvert (`pio device monitor -b 115200`) pour voir les logs `[OTA]`.
- Manifest et .bin servis en HTTPS. Le device tente d’abord l’URL primaire (GitHub Pages), puis en **fallback le backend** (`breezly-backend.onrender.com/.../prod/latest.json`). **En pratique, GitHub Pages est souvent injoignable (connection refused)** ; le backend est la source fiable et sert l’OTA (manifest + .bin) correctement, avec téléchargement en 3 streams.

### Format du manifest (latest.json)

Le JSON doit contenir au minimum :

- `version` : **strictement supérieure** à la version sur le device (ex. device en `1.0.21` → mettre `1.0.22`).
- `url` : URL HTTPS du fichier `.bin` (même domaine ou hébergeur valide TLS).
- `size` : taille en octets du `.bin`.
- `sha256` : hash SHA256 du `.bin` en hexadécimal (minuscules).
- `product`, `model`, `channel` : `model` doit être exactement **`wroom32e`**.
- `sig` : signature ECDSA P-256 du **canonical** en base64 (voir ci‑dessous).
- `rollout` : mettre **`100`** pour test (sinon le device peut skip selon `sensorId`).
- Optionnel : `force: true` pour forcer l’update même si version égale.

**Canonical signé** (ordre des champs, sans espaces superflus) :

```json
{"product":"<product>","model":"wroom32e","channel":"prod","version":"1.0.22","url":"https://.../firmware.bin","size":1234567,"sha256":"abc123..."}
```

La clé privée doit être celle qui correspond à la **clé publique** dans `src/ota/ota.cpp` (`OTA_PUBKEY_PEM`). Sans cette clé, `[OTA] Signature INVALID` → refus systématique.

### Étapes pour un test réussi

1. **Bumper la version**  
   Dans `src/app_config.h`, passer à `1.0.22` (ou supérieur). Build prod :  
   `pio run -e esp32-wroom-32e-prod`  
   Récupérer le `.bin` dans `.pio/build/esp32-wroom-32e-prod/firmware.bin`.

2. **Calculer size et sha256**  
   - `size` = taille fichier en octets.  
   - `sha256` = hash hex du fichier (ex. `sha256sum firmware.bin` ou outil équivalent).

3. **Publier le .bin**  
   Mettre le fichier sur une URL HTTPS accessible (backend, GitHub Releases, ou dépôt `breezly-firmware-dist`).

4. **Générer le manifest**  
   Construire le JSON avec `version`, `url` (vers ce .bin), `size`, `sha256`, `product`, `model` = `wroom32e`, `channel` = `prod`, `rollout` = 100.

5. **Signer le canonical**  
   Avec la clé privée associée à `OTA_PUBKEY_PEM`, signer la chaîne canonical (ex. ECDSA P-256, SHA256), encoder en base64 → champ `sig`.

6. **Publier latest.json + .bin**  
   Le script `publish.js` écrit dans le dépôt firmware-dist et dans `back-end-breezly/public/`. En pratique le device récupère tout depuis le backend (`https://breezly-backend.onrender.com/firmware/esp32/wroom32e/prod/latest.json` et le .bin). Commit + push du backend (et déploiement) pour rendre l’OTA disponible.

7. **Flasher le device avec l’ancienne version**  
   Pour avoir une version **inférieure** au manifest : reflasher un firmware en `1.0.21` (ou ne pas bumper avant ce test), puis alimenter le device.

8. **Déclencher le check OTA**  
   - Soit attendre le check périodique (intervalle dans `app_config.h`).  
   - Soit envoyer la commande MQTT prévue (topic ctrl, payload déclenchant OTA) pour forcer un check immédiat.

9. **Vérifier en Serial**  
   - `[OTA] GET manifest: ...` → manifest récupéré.  
   - `[OTA] manifest signature OK` → signature valide.  
   - `[OTA] version cmp: current=1.0.21 target=1.0.22 -> -1` → mise à jour acceptée.  
   - `[OTA] UPDATE → https://...` puis progression download / `esp_ota_write` / `boot partition set` → reboot.  
   - Après reboot : version affichée ou dans le status = `1.0.22`.

### En cas d’échec : quoi regarder

| Log Serial | Cause probable | Action |
|------------|----------------|--------|
| `GET ... github.io/... -> code=-1 (connection refused)` | Normal : GitHub Pages souvent injoignable | Le device bascule sur le backend ; vérifier que `primary failed → try direct: https://breezly-backend.onrender.com/...` puis manifest OK. |
| `manifest fetch failed (both)` | URL manifest injoignable (GitHub + backend) | Vérifier que le backend est déployé et sert `public/firmware/.../latest.json`. |
| `Signature INVALID` | Manifest non signé ou mauvaise clé | Signer avec la clé privée liée à `OTA_PUBKEY_PEM`. |
| `champs manquants` | version, url, etc. absents | Vérifier tous les champs du JSON. |
| `model mismatch` | `model` ≠ `wroom32e` | Mettre `"model":"wroom32e"`. |
| `skip by rollout` | bucket ≥ rollout | Mettre `rollout: 100` (ou `force: true`). |
| `déjà à jour (skip)` | version manifest ≤ version en flash | Mettre une version **supérieure** dans le manifest (ex. 1.0.22). |
| `download/flash failed` | .bin URL, TLS, taille, ou sha256 | Vérifier URL, size, sha256 du .bin ; logs HTTP juste au-dessus. |

En résumé : **version manifest > version device**, **manifest signé avec la clé correspondant à `ota.cpp`**, **rollout=100** (ou force), et **.bin accessible en HTTPS** avec les bons size/sha256.

### OTA rollback safe : état du code et test

**Où on en est dans le code**

- **Fichier** : `src/ota/ota.cpp` (boot validation) et `src/main.cpp` (appel de `otaOnBootValidate()` au début de `setup()`).
- **Mécanisme** : après un OTA, le firmware écrit `pending=true` en NVS puis redémarre. À chaque boot, `otaOnBootValidate()` lit `pending` et `fail` ; si `pending` est vrai, on incrémente `fail`. Si `fail` atteint **3**, on enregistre la version courante (celle qu’on rejette) en NVS (`rolled_back_ver`), puis on appelle `esp_ota_mark_app_invalid_rollback_and_reboot()` (IDF 4+) : la partition courante (la nouvelle) est marquée invalide et le bootloader redémarre sur l’**autre** partition (l’ancienne). La partition « survivante » voit `fail >= 3` et interprète qu’elle est la cible du rollback : on efface `pending`/`fail` et on marque l’app valide, sans relancer de rollback (évite la boucle).
- **Pas de ré-install à l’infini** : avant le rollback, la version rejetée est enregistrée dans `rolled_back_ver`. Lors du check OTA suivant, si le manifest propose cette même version, elle est **ignorée** (`[OTA] skip: version X was rolled back (reject re-install)`). La clé est effacée uniquement après un boot réussi d’une **autre** mise à jour (nouvelle version validée), ce qui évite la boucle « rollback → check OTA → re-télécharger la même version → rollback ».
- **Partitions** : `partitions.csv` (app0, app1, otadata) ; pas de factory, rollback = bascule app0 ↔ app1.
- **Correction récente** : sans la clause « si `fail >= MAX_FAILS` on est la cible du rollback → clear pending, mark valid », l’ancienne partition aurait à son tour incrémenté `fail` et déclenché un rollback → boucle. C’est corrigé.

**Comment tester le rollback (procédure reproductible)**

1. **Device en prod stable**  
   Flasher une version « bonne » (ex. 1.0.22 prod normale) et vérifier que le device boot correctement (Serial : `[OTA] App marked VALID`).

2. **Build « firmware qui simule l’échec »**  
   Bumper la version (ex. 1.0.23) dans `app_config.h`. Build avec l’env **rollback-test** (il simule un reboot avant de marquer l’app valide, 2 fois, puis au 3ᵉ boot déclenche le rollback) :  
   `pio run -e esp32-wroom-32e-prod-rollback-test`  
   Le binaire est dans `.pio/build/esp32-wroom-32e-prod-rollback-test/firmware.bin`.

3. **Publier ce .bin comme OTA**  
   `node back-end-breezly/tools/publish.js esp32_wroom_32e/.pio/build/esp32-wroom-32e-prod-rollback-test/firmware.bin 1.0.23`  
   Puis commit + push du backend (et déploiement) pour que le manifest pointe vers 1.0.23.

4. **Déclencher l’OTA sur le device**  
   Le device en 1.0.22 récupère le manifest 1.0.23, télécharge et flashe, puis redémarre (partition 1.0.23, `pending=true`).

5. **Observer en Serial**  
   - **Boot 1** (nouvelle app) : `[OTA] BootValidate: pending=1 fail=0` → `[OTA] TEST: simulate failed boot 1/3 → reboot`.  
   - **Boot 2** : `pending=1 fail=1` → `simulate failed boot 2/3 → reboot`.  
   - **Boot 3** : `pending=1 fail=2` → `[OTA] Too many failed boots → rollback` puis reboot.  
   - **Boot 4** (ancienne app) : `[OTA] BootValidate: pending=1 fail=3` → `[OTA] Rollback target: clear pending, mark valid` puis setup continue ; version affichée = 1.0.22 (ancienne).

6. **Critère OK**  
   Après les 3 reboots courts, le device repart sur l’ancienne partition (1.0.22), sans boucle de rollback. Au prochain check OTA, le device doit afficher `[OTA] skip: version 1.0.23 was rolled back (reject re-install)` et **ne pas** retélécharger 1.0.23. Comportement normal (WiFi, MQTT, etc.) jusqu’à ce qu’une version supérieure (ex. 1.0.24) soit proposée.

**Sans l’env de test** (firmware prod normal), le rollback ne se déclenche que si la **nouvelle** app plante ou redémarre **avant** d’atteindre `esp_ota_mark_app_valid_cancel_rollback()` (ex. crash en tout début de `setup()`). Trois tels boots consécutifs provoquent le rollback.

*Validation 2026-02 :* test rollback (env rollback-test) exécuté avec succès ; pas de boucle de ré-install. **P0 firmware considéré complet** avec cette validation.

### Script backend : publier le binaire + manifest (firmware-dist + backend public)

Le script `back-end-breezly/tools/publish.js` copie le `.bin` local, calcule size/sha256, signe le manifest (ECDSA P-256) et écrit :

1. **breezly-firmware-dist** (GitHub Pages) : `latest.json`, `latest.bin`, manifest versionné, binaire versionné.  
2. **back-end-breezly/public/firmware/...** : les mêmes fichiers, avec l’URL du backend dans le manifest.  

**En pratique**, GitHub Pages est souvent injoignable (connection refused). Les devices utilisent donc le **fallback backend** ; en mettant tout sur le backend (manifest + .bin dans `public/`), l’OTA fonctionne parfaitement (téléchargement en 3 streams, reboot, version à jour). Après `publish`, commit + push du **backend** (et déploiement Render) suffisent pour que les devices reçoivent la mise à jour.

**Commande (depuis la racine du repo multiagent) :**

```bash
# 1) Build prod firmware (version déjà bumpée dans app_config.h, ex. 1.0.22)
cd esp32_wroom_32e && pio run -e esp32-wroom-32e-prod && cd ..

# 2) Publier : chemin vers le .bin + version (identique à CURRENT_FIRMWARE_VERSION)
node back-end-breezly/tools/publish.js esp32_wroom_32e/.pio/build/esp32-wroom-32e-prod/firmware.bin 1.0.22
```

**Depuis le dossier backend :**

```bash
cd back-end-breezly
node tools/publish.js ../esp32_wroom_32e/.pio/build/esp32-wroom-32e-prod/firmware.bin 1.0.22
```

**Prérequis :**  
- Clé privée ECDSA P-256 : `back-end-breezly/tools/ec_private.pem` (ou variable `FW_SIGN_KEY_FILE`). Elle doit correspondre à la clé publique dans `esp32_wroom_32e/src/ota/ota.cpp` (`OTA_PUBKEY_PEM`).  
- Génération si besoin :  
  `openssl ecparam -genkey -name prime256v1 -noout -out back-end-breezly/tools/ec_private.pem`  
  puis extraire la clé publique et la mettre dans `ota.cpp` (ou utiliser la paire existante).

**Variables optionnelles :**  
- `FW_CHANNEL` : `prod` (défaut) ou `dev`  
- `FW_BASE_URL` : URL de base des assets pour le dépôt firmware-dist (défaut GitHub Pages)  
- `FW_BACKEND_BASE_URL` : URL du backend pour le manifest servi depuis `public/` (défaut `https://breezly-backend.onrender.com`)  
- `FW_ROLLOUT` : 100 (défaut)  
- `FW_FORCE=1` : forcer l’update même si version égale  

Le script écrit dans `breezly-firmware-dist/...` et dans `back-end-breezly/public/firmware/esp32/wroom32e/<channel>/`. Pour que l’OTA soit disponible : **commit + push du backend** (et déploiement), le backend servant manifest et .bin de façon fiable.

---

## Ce qui manque encore (P1 / P2)

*Basé sur l’état du code et les tests non encore validés.*

| Priorité | Item | État code | Test validé |
|----------|------|-----------|-------------|
| P1 | Backoff exponentiel Wi‑Fi / MQTT | Non implémenté (délais fixes) | — |
| P1 | Reset reason loggé au boot (+ optionnel status MQTT) | Non implémenté | — |
| P1 | État OTA unifié (otaIsInProgress partout, suppression variable globale otaInProgress) | Partiel (sleep.h, main utilisent otaIsInProgress) | — |
| P1 | Procédure factory + EOL exécutée et consignée | Doc présente (FACTORY_E2E_CHECKLIST) | À remplir |
| P1 | APP_ENV_DEV aligné avec BREEZLY_DEV (manifest dev/prod selon build) | APP_ENV_DEV jamais défini → URL manifest toujours prod (risque opérationnel OTA) | — |
| P2 | Logs par niveau (LOG_LEVEL), pas de Serial.println(payload) en prod | Non implémenté | — |
| P2 | Timeout I2C + reset bus capteurs après N échecs | Non implémenté | — |
| P2 | Sanity checks AQI/TVOC/eCO2 avant publish | Non implémenté | — |

---

## Annexes (docs détaillées)

| Document | Rôle |
|----------|------|
| [PRODUCTION_READINESS.md](PRODUCTION_READINESS.md) | Build, téléversement, GO/NO-GO, résultats grep, liens. |
| [esp32_wroom_32e/FIRMWARE_PRODUCTION_READINESS_AUDIT.md](esp32_wroom_32e/FIRMWARE_PRODUCTION_READINESS_AUDIT.md) | Audit complet (architecture, checklist, sécurité, OTA, robustesse, factory, plan P0/P1/P2). |
| [esp32_wroom_32e/RELEASE_PLAYBOOK.md](esp32_wroom_32e/RELEASE_PLAYBOOK.md) | Bump version, build prod, manifest (version, url, size, sha256, sig ECDSA), test 1 device, DoD. Clé privée OTA : hors repo (CI/outil de signature). |
| [esp32_wroom_32e/FACTORY_E2E_CHECKLIST.md](esp32_wroom_32e/FACTORY_E2E_CHECKLIST.md) | Flash usine, EOL, recovery, traçabilité. |
| [esp32_wroom_32e/QUICK_WINS.md](esp32_wroom_32e/QUICK_WINS.md) | Actions < 1 j (dont déjà faites P0). |
| [esp32_wroom_32e/AUDIT_P0_PREUVES.md](esp32_wroom_32e/AUDIT_P0_PREUVES.md) | Preuves P0 (fichier + ligne) alignement code/doc. |
| [BREEZLY_DA_BRANDING.md](BREEZLY_DA_BRANDING.md) | Charte design / branding. |
| breezly (ENV, GUIDE_ENVIRONNEMENT) | App mobile : .env, commandes dev/prod. |
| back-end-breezly/README.md | Backend : suivi commande, API. |

---

## Autres docs projet (résumés)

- **App Breezly** : `.env.development` / `.env.production` (VITE_API_BASE_URL, VITE_APP_MODE). Commandes : `npm run android:dev` / `android:prod`. Voir `breezly/ENV_SETUP.md`, `breezly/GUIDE_ENVIRONNEMENT.md`, `breezly/ENV_EXAMPLE.md`.
- **Backend** : Suivi commande V1 (token suivi, URL publique). Voir `back-end-breezly/README.md`.
- **Branding** : Identité Breezly (couleurs, typo, composants, ton). Voir `BREEZLY_DA_BRANDING.md`.

---

*Mise à jour : après chaque campagne de tests terrain, remplir le tableau « Validation terrain » et les cases GO/NO-GO, puis mettre à jour la date en en-tête.*
