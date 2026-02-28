# Procédure Factory — Flash usine + EOL (fin de ligne)

**Document de référence pour la mise en production et la traçabilité des appareils Breezly.**  
Procédure **industrielle** (hub USB multi-ports, une commande, journal EOL rempli automatiquement) ou manuelle (un appareil). Consignation obligatoire pour commercialisation.

---

## 1. Prérequis

| # | Prérequis | Vérification |
|---|-----------|---------------|
| 1.1 | Poste avec **PlatformIO** (CLI) et **Python 3** | `pio --version` et `python --version` OK |
| 1.2 | **secrets.ini** dans `esp32_wroom_32e/` (copie de `secrets.ini.example`) : `custom_device_key_b64`, `custom_factory_token`, `custom_mqtt_user`, `custom_mqtt_pass` | Fichier présent ; aucune valeur vide pour le build prod |
| 1.3 | **Hub USB** (ex. 10 ports) avec PCBs branchés, ou un seul câble USB pour procédure manuelle | Ports visibles (Device Manager / `ls /dev/tty*`) |
| 1.4 | Accès réseau vers l’API backend (enregistrement device après flash) | `post_upload_register.py` appelle `custom_api_url` (prod : `https://breezly-backend.onrender.com`) |

**Critère KO :** Si 1.2 non satisfait → build refusé ou post-upload en erreur (FACTORY_TOKEN / DEVICE_KEY_B64 manquants).

---

## 2. Procédure industrielle (recommandée) — `flash_fleet.py`

**Une seule commande :** brancher tous les PCBs sur le hub USB, lancer le script → build une fois, upload en parallèle sur tous les ports détectés, enregistrement API par appareil, **journal EOL rempli automatiquement** (conforme traçabilité commerciale).

### 2.1 Commande

Depuis le répertoire **`esp32_wroom_32e`** (ou depuis la racine du repo si le script est invoqué avec le bon `cwd`) :

```bash
cd esp32_wroom_32e
python scripts/flash_fleet.py --env esp32-wroom-32e-prod --jobs 5 --variant STD --operator "Nom Opérateur"
```

*(Remplacer `STD` par `PREMIUM` si besoin ; `--variant` est recommandé pour remplir la colonne Variant du journal EOL.)*

Sous Windows, si `python` pointe vers le bon interpréteur :

```bash
cd esp32_wroom_32e
python scripts\flash_fleet.py --env esp32-wroom-32e-prod --jobs 5 --operator "Nom Opérateur"
```

### 2.2 Comportement

| Étape | Action |
|-------|--------|
| 1 | **Build** une seule fois (firmware prod). |
| 2 | **Détection des ports** : tous les devices USB (filtre par défaut : `cp210`, adaptatif CH340/CP210). |
| 3 | **Upload en parallèle** sur chaque port (`--jobs` pour limiter le parallélisme, ex. 5 pour un hub 10 ports). |
| 4 | Pour chaque port : après upload, **post_upload_register.py** (hook PlatformIO) enregistre le device sur l’API (`external_id = PROV_{MAC}`). |
| 5 | **Journal EOL** : après chaque upload (succès ou échec), une ligne est **ajoutée automatiquement** dans `docs/EOL_LOG.csv` (ou fichier fourni par `--eol-log`). |

### 2.3 Options principales

| Option | Description | Défaut |
|--------|-------------|--------|
| `--env` | Environnement PlatformIO (prod recommandé) | requis |
| `--jobs` | Nombre d’uploads en parallèle (2–10 selon hub) | 3 |
| `--eol-log` | Fichier CSV du journal EOL | `docs/EOL_LOG.csv` |
| `--operator` | Nom de l’opérateur (traçabilité) | variable d’env `EOL_OPERATOR` ou vide |
| `--no-eol` | Désactive l’écriture du journal EOL (flash seul) | — |
| `--no-build` | Pas de build (upload uniquement, après un build précédent) | — |
| `--include` | Filtre ports (ex. `cp210 ch340`) | `cp210` |
| `--ports` | Liste explicite de ports (ex. `COM3 COM4 COM5`) | auto-détection |
| `--variant` | **Recommandé.** Variant matériel : `STD` ou `PREMIUM` (écrit dans la colonne Variant du journal EOL + transmis au backend). | — |
| `--pio-exe` | Chemin vers `platformio.exe` (Windows) | chemin par défaut dans le script |

### 2.4 Format du journal EOL (auto-rempli)

Le fichier `docs/EOL_LOG.csv` (ou celui donné par `--eol-log`) est créé avec l’en-tête suivant, puis **une ligne par appareil** à chaque run :

| Colonne | Contenu |
|---------|---------|
| Date | YYYY-MM-DD (heure d’écriture de la ligne) |
| Heure | HH:MM:SS |
| MAC | Adresse MAC avec deux-points (ex. 80:BA:D0:21:57:88) |
| external_id | PROV_{MAC 12 hex} (ex. PROV_80BAD0215788) |
| Version_FW | Version lue depuis `src/app_config.h` (ex. 1.0.25) |
| **Variant** | **STD ou PREMIUM** (valeur de `--variant` ; obligatoire en prod pour traçabilité) |
| **Port** | Port utilisé pour le flash (ex. COM8, /dev/ttyUSB0) — traçabilité hub USB |
| EOL_resultat | OK (upload + enregistrement API OK) ou KO (upload_failed / register_failed_or_no_external_id) |
| Operateur | Valeur de `--operator` ou `EOL_OPERATOR` |
| Remarques | Vide si OK ; sinon court motif d’échec |

Le fichier est **créé** au premier run (avec en-tête) puis **complété par ajout** à chaque exécution. Il est listé dans `.gitignore` (`docs/EOL_LOG.csv`) pour ne pas committer les données de production.

### 2.5 Critères de succès

- [ ] Tous les ports listés en `✅ PORT OK` en sortie.
- [ ] Message final : `Terminé. OK=N / FAIL=0`.
- [ ] Ligne `Journal EOL mis à jour: ...` en fin de run.
- [ ] Chaque appareil apparaît dans le backend (vérification optionnelle) et une ligne correspondante dans le CSV EOL avec `EOL_resultat=OK`.

En cas d’échec sur un ou plusieurs ports : les lignes KO sont tout de même écrites dans le journal (traçabilité des échecs). Corriger (câble, port, API) et relancer si besoin.

---

## 3. Procédure manuelle (un seul appareil)

À utiliser si un seul PCB est branché ou si `flash_fleet.py` n’est pas utilisé.

### 3.1 Build

```bash
cd esp32_wroom_32e
pio run -e esp32-wroom-32e-prod
```

- [ ] Build sans erreur, aucun secret en clair dans les logs.

### 3.2 Upload (remplacer `COMx` par le port réel)

```bash
pio run -e esp32-wroom-32e-prod -t upload --upload-port COMx
```

- [ ] Upload OK ; post-upload : `[post-upload] external_id pour provision: PROV_XXXXXXXXXXXX` ; enregistrement API réussi.
- [ ] **Consignation :** ajouter manuellement une ligne dans `docs/EOL_LOG.csv` (ou utiliser `docs/FACTORY_EOL_LOG_TEMPLATE.csv`) avec Date, Heure, MAC, external_id, Version_FW, EOL_resultat, Operateur, Remarques.

### 3.3 Vérifications post-flash (optionnel, si Serial dispo)

```bash
pio device monitor -b 115200 --port COMx
```

- [ ] Premier boot stable ; BLE visible avec nom `PROV_*` ; device prêt pour provisioning.

---

## 4. Tests fin de ligne (EOL) — par appareil

À exécuter pour **chaque** appareil après flash (ou sur échantillon si procédure statistique validée). Avec **flash_fleet**, la traçabilité (EOL_resultat OK/KO, MAC, external_id, version) est déjà dans le journal ; les tests ci-dessous complètent la qualification (optionnel en lot).

| # | Test | Méthode | Critère OK | Critère KO | Temps estimé |
|---|------|---------|------------|------------|---------------|
| EOL-1 | Alimentation | Brancher USB ou alimentation | LED ou comportement attendu au boot | Aucune réaction, surchauffe | 10 s |
| EOL-2 | Boot | Reset ou power-on ; observer 2 min | Pas de reboot en boucle ; Serial : `[BOOT] Setup terminé` si dispo | Reset répété, panic | 30 s – 2 min |
| EOL-3 | BLE discoverable | App ou scan BLE (ex. nRF Connect) | Nom `PROV_*` visible, service 60f8a11f-... | Invisible ou mauvais nom | 30 s |
| EOL-4 | I2C capteurs (optionnel) | Boot + publish si WiFi/MQTT configurés | Pas d’erreur AHT21/ENS160 en Serial | Erreurs I2C répétées | — |
| EOL-5 | Stabilité 2 min | Laisser tourner 2 min après boot | Pas de reset ; BLE reste visible si pas de WiFi | Crash, reset inattendu | 2 min |
| EOL-6 | Traçabilité | MAC / external_id + résultat EOL | Ligne dans le journal EOL (automatique avec flash_fleet) | Non consigné | — |

**Temps total EOL par appareil (manuel) :** environ 3–5 min. Avec **flash_fleet**, EOL-6 est couvert automatiquement ; EOL-1 à EOL-5 restent optionnels par échantillon ou en cas de doute.

---

## 5. Procédure de recovery (flash de secours)

En cas de firmware corrompu ou appareil « brické » :

1. Disposer du **.bin** de firmware (release stable), ex. :  
   `esp32_wroom_32e/.pio/build/esp32-wroom-32e-prod/firmware.bin`
2. Brancher l’ESP32, identifier le port COM.
3. Lancer l’upload (adapter `COMx`) :

```bash
cd esp32_wroom_32e
pio run -e esp32-wroom-32e-prod -t upload --upload-port COMx
```

Ou avec esptool :

```bash
python -m esptool --port COMx --baud 921600 write_flash 0x10000 .pio/build/esp32-wroom-32e-prod/firmware.bin
```

- [ ] Documenter en interne le **chemin du .bin de secours** et la **version firmware** associée.

---

## 6. Consignation EOL et traçabilité

**Obligatoire pour commercialisation :** chaque appareil flashé doit être consigné.

- **Avec flash_fleet.py :** le journal `docs/EOL_LOG.csv` est **rempli automatiquement** (Date, Heure, MAC, external_id, Version_FW, EOL_resultat, Operateur, Remarques). Conserver ce fichier (ou son export) pour traçabilité et rappel éventuel.
- **Sans flash_fleet :** utiliser le template **`docs/FACTORY_EOL_LOG_TEMPLATE.csv`** (copier, renommer si besoin, remplir une ligne par appareil).

Identifiants device : **external_id** = `PROV_{MAC}` (12 caractères hex) ; **Version FW** = `CURRENT_FIRMWARE_VERSION` dans `src/app_config.h`.

---

## 7. Références

| Document / ressource | Rôle |
|---------------------|------|
| **`scripts/flash_fleet.py`** | **Point central** : flash flotte en parallèle + remplissage automatique du journal EOL. Une commande depuis `esp32_wroom_32e`. |
| [DOCUMENTATION.md](DOCUMENTATION.md) | Vue d’ensemble firmware, validation terrain, GO/NO-GO |
| [FIRMWARE_PRODUCTION_READINESS_AUDIT.md](FIRMWARE_PRODUCTION_READINESS_AUDIT.md) | Audit §7 Factory & industrialisation |
| [RELEASE_PLAYBOOK.md](RELEASE_PLAYBOOK.md) | Bump version, build prod, OTA, factory |
| `scripts/post_upload_register.py` | Enregistrement device après upload (hook PlatformIO, external_id, API) |
| `scripts/pre_build_devkey.py`, `pre_build_flashsig.py`, `pre_build_mqtt_secrets.py` | Prérequis build (secrets hors repo) |
| `secrets.ini.example` | Template pour `secrets.ini` (ne pas committer `secrets.ini`) |
| `docs/FACTORY_EOL_LOG_TEMPLATE.csv` | Template manuel du journal EOL (si pas d’usage de flash_fleet) |
| `docs/EOL_LOG.csv` | Journal EOL **généré automatiquement** par `flash_fleet.py` (dans .gitignore) |
