# Release Playbook — Firmware Breezly ESP32

Procédure minimale pour une release firmware commerciale.

---

## 1. Où est la version

- **Fichier:** `src/app_config.h`
- **Définition:** `#define CURRENT_FIRMWARE_VERSION "x.y.zz"`
- **Règle:** Incrémenter avant chaque release (ex. `1.0.21` → `1.0.22` pour correctif, `1.1.0` pour feature).

---

## 2. Bump version

1. Ouvrir `src/app_config.h`.
2. Modifier `CURRENT_FIRMWARE_VERSION` (ex. `"1.0.22"`).
3. Commit dédié: `git commit -m "chore(fw): bump version to 1.0.22"`.

---

## 3. Build prod

1. Vérifier que `secrets.ini` est présent et rempli (device key, factory token, MQTT user/pass).  
   Si absent: `cp secrets.ini.example secrets.ini` puis remplir les valeurs.
2. Build environnement prod:

```bash
cd esp32_wroom_32e
pio run -e esp32-wroom-32e-prod
```

3. Vérifier: pas d’erreur de build; binaire dans `.pio/build/esp32-wroom-32e-prod/firmware.bin`.

---

## 4. Génération / publication du manifest OTA

Le device récupère le manifest depuis l’URL configurée dans `app_config.h` (ex. GitHub Pages ou backend). Pour une release:

1. Générer ou mettre à jour le manifest (JSON) avec:
   - `version`: même que `CURRENT_FIRMWARE_VERSION`
   - `url`: URL du fichier .bin (HTTPS)
   - `size`, `sha256`, `sig` (signature ECDSA du manifest canonique)
   - `product`, `model`, `channel` cohérents (ex. `wroom32e`, `prod`)

2. Publier le .bin et le manifest sur l’hébergeur (ex. dépôt `breezly-firmware-dist`, ou backend).

3. Signer le manifest avec la clé privée associée à la clé publique embarquée dans `ota.cpp` (`OTA_PUBKEY_PEM`).

Référence: dépôt / outil de distribution firmware (hors ce repo).

---

## 5. Test sur 1 device

1. Flasher un device avec le binaire de la release:

```bash
pio run -e esp32-wroom-32e-prod -t upload
```

2. Vérifier:
   - Boot sans erreur
   - WiFi + MQTT connectés (si déjà provisionné)
   - Au moins une trame capteur reçue côté backend
   - (Optionnel) Déclencher une OTA depuis le manifest et vérifier mise à jour + reboot

3. Si OTA test: vérifier que la version rapportée (boot MQTT, ou status) correspond à la nouvelle version.

---

## 6. Rollout (si supporté)

- Le firmware utilise un champ `rollout` (%) dans le manifest OTA (bucket par `sensorId`).
- Pour une release progressive: mettre d’abord un `rollout` faible (ex. 10%), vérifier en production, puis augmenter jusqu’à 100%.

---

## 7. Definition of Done (release)

Une release est considérée “done” quand:

- [ ] Version bumpée dans `src/app_config.h` et commitée.
- [ ] Build prod `pio run -e esp32-wroom-32e-prod` OK.
- [ ] Manifest OTA généré, signé et publié (url, version, sha256, sig).
- [ ] Test sur au moins 1 device physique: boot, connexion, publish capteur (et optionnellement OTA).
- [ ] Binaire et version tagués dans le dépôt (ex. `git tag v1.0.22`).
- [ ] Pas de secret en clair dans le dépôt (build avec `secrets.ini` ou env).

---

## Références

- Audit: [FIRMWARE_PRODUCTION_READINESS_AUDIT.md](FIRMWARE_PRODUCTION_READINESS_AUDIT.md)
- Factory / EOL: [FACTORY_E2E_CHECKLIST.md](FACTORY_E2E_CHECKLIST.md)
