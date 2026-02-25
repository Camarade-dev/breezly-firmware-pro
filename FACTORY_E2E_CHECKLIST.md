# Checklist Factory — Flash usine + EOL

À utiliser en fin de ligne (EOL) ou après flash usine pour chaque appareil.

---

## 1. Prérequis

- [ ] Poste avec PlatformIO / Python 3
- [ ] `secrets.ini` ou variables d’env définies: `custom_device_key_b64`, `custom_factory_token`, (optionnel) `custom_api_url`
- [ ] Câble USB fonctionnel, port COM identifié
- [ ] Accès réseau pour enregistrement API (post_upload_register)

---

## 2. Flash usine

### 2.1 Build

```bash
cd esp32_wroom_32e
pio run -e esp32-wroom-32e-prod
```

- [ ] Build sans erreur
- [ ] Aucun secret affiché dans les logs (vérifier que secrets viennent de secrets.ini / env)

### 2.2 Upload

```bash
pio run -e esp32-wroom-32e-prod -t upload
```

- [ ] Upload OK (921600 baud)
- [ ] Post-upload: script `post_upload_register.py` exécuté sans erreur
- [ ] API retourne succès (device enregistré avec `external_id = PROV_{MAC}`)

### 2.3 Vérifications post-flash (optionnel, si Serial dispo)

- [ ] Premier boot: pas de panic / exception
- [ ] BLE advertising actif (nom du type `PROV_xxxxxxxxxxxx`)
- [ ] Aucune config WiFi en NVS (device prêt pour provisioning)

---

## 3. Tests fin de ligne (EOL) — par appareil

| # | Test | Méthode | Critère OK | Temps estimé |
|---|------|---------|------------|--------------|
| 1 | Alimentation | Brancher USB / alimentation | LED ou comportement attendu au boot | 10 s |
| 2 | Boot | Reset ou power-on | Pas de reboot en boucle; Serial (si dispo): "[BOOT] Setup terminé" ou équivalent | 30 s |
| 3 | BLE discoverable | App/scan BLE (ex. nRF Connect) | Nom `PROV_*` visible, service 60f8a11f-... | 30 s |
| 4 | I2C capteurs (optionnel) | Déjà couvert par boot + publish si WiFi/MQTT OK | Pas d’erreur "AHT21 initialization failed" / "Échec ENS160" en Serial | — |
| 5 | Pas de crash 2 min | Laisser tourner 2 min après boot | Pas de reset; si WiFi non configuré, BLE reste visible | 2 min |
| 6 | Traçabilité | Noter MAC ou external_id | Enregistrement API OK (voir 2.2) | — |

**Temps total EOL par appareil (manuel):** ~3–5 min.

### Optionnel (si environnement WiFi + MQTT disponible)

- [ ] Provisioning BLE complet (SSID/mot de passe ou EAP)
- [ ] Connexion WiFi puis MQTT
- [ ] Au moins une trame `capteurs/qualite_air` reçue côté backend

---

## 4. Procédure flash de secours (recovery)

En cas de firmware corrompu ou brick:

1. Tenir le bon .bin de firmware (release connue stable).
2. Ouvrir un terminal sur le port COM de l’ESP32.
3. Exécuter (adapter `COMx` et chemin du .bin):

```bash
pio run -e esp32-wroom-32e-prod -t upload --upload-port COMx
```

Ou avec esptool directement:

```bash
python -m esptool --port COMx --baud 921600 write_flash 0x10000 .pio/build/esp32-wroom-32e-prod/firmware.bin
```

- [ ] Documenter le chemin du .bin de secours et la version associée.

---

## 5. Numéros de série / traçabilité

- **Identifiant device:** `PROV_{MAC en hex, 12 car, ordre reverse}` (ex. `PROV_A1B2C3D4E5F6`).
- **Enregistrement:** fait par `post_upload_register.py` vers l’API (backend).
- Recommandation: en usine, consigner dans un tableau (ou outil) pour chaque appareil: date flash, MAC, external_id, résultat EOL (OK / KO).

---

## 6. Références

- Audit complet: [FIRMWARE_PRODUCTION_READINESS_AUDIT.md](FIRMWARE_PRODUCTION_READINESS_AUDIT.md) (§7 Factory & Industrialisation).
- Scripts: `scripts/pre_build_devkey.py`, `scripts/pre_build_flashsig.py`, `scripts/post_upload_register.py`.
