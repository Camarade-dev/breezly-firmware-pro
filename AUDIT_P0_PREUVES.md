# Preuves P0 — alignement code / doc (2026-02)

Vérification que la documentation reflète le code réel.

---

## P0-1 : Secrets hors firmware

| Vérification | Preuve (fichier + repère) |
|--------------|---------------------------|
| secrets.ini.example présent | `esp32_wroom_32e/secrets.ini.example` (clés vides, [env] custom_device_key_b64, custom_factory_token, custom_mqtt_user, custom_mqtt_pass). |
| secrets.ini gitignored | `esp32_wroom_32e/.gitignore` L8 : `secrets.ini`. |
| platformio.ini charge secrets, sans valeurs | `esp32_wroom_32e/platformio.ini` L2 : `extra_configs = secrets.ini` ; L55-56 : commentaire uniquement, pas de valeur. |
| MQTT creds non hardcodés | `esp32_wroom_32e/src/net/mqtt_bus.cpp` L17 `#include "mqtt_secrets.h"`, L31-32 `MQTT_USER = MQTT_SECRET_USER`, `MQTT_PASS = MQTT_SECRET_PASS`. Aucun littéral. |
| mqtt_secrets.h généré, gitignored | `esp32_wroom_32e/.gitignore` L10 : `src/net/mqtt_secrets.h`. Généré par `scripts/pre_build_mqtt_secrets.py`. |
| devkey.h généré, gitignored | `esp32_wroom_32e/.gitignore` L9 : `src/core/devkey.h`. Généré par `scripts/pre_build_devkey.py`. |
| Scripts pre-build présents | `scripts/pre_build_devkey.py`, `scripts/pre_build_mqtt_secrets.py`, `scripts/pre_build_flashsig.py` ; `platformio.ini` L45-48. |

**Conclusion P0-1 :** Conforme. Aucun secret en clair dans le repo versionné.

---

## P0-2 : OTA sans setInsecure

| Vérification | Preuve (fichier + repère) |
|--------------|---------------------------|
| Aucun setInsecure dans ota.cpp | `esp32_wroom_32e/src/ota/ota.cpp` : grep ne retourne que `setCACert` (L113, L284). |
| TLS manifest + .bin via CA | `ota.cpp` L20 `#include "ca_bundle.h"` ; L113 `wcs.setCACert(CA_BUNDLE_PEM)` (httpGetToString) ; L284 `tls.setCACert(CA_BUNDLE_PEM)` (downloadAndFlashWithHTTPClientInner). |
| Où est le PEM | `esp32_wroom_32e/src/ca_bundle.h` (et `src/certs/ca_bundle.h`) : `static const char CA_BUNDLE_PEM[] PROGMEM = R"PEM(...)"` — compilé dans le binaire. |
| embed_txtfiles (platformio) | `platformio.ini` L51-53 : `ca_rezoleo.pem`, `hivemq_ca.pem` — utilisés pour EAP (wifi_enterprise) et broker MQTT, pas pour OTA. OTA utilise uniquement `ca_bundle.h`. |
| setInsecure restant (hors OTA) | `esp32_wroom_32e/src/net/sntp_utils.cpp` L57 : `c.setInsecure()` pour fallback sync horloge (HEAD google.com). Pas OTA ; surface limitée (lecture en-tête Date). |

**Conclusion P0-2 :** Conforme pour OTA. TLS vérifié pour manifest et .bin. Un seul setInsecure restant : sntp_utils (fallback Date), documenté dans GO/NO-GO.

---

## P0-3 : Release discipline

| Vérification | Preuve (fichier + repère) |
|--------------|---------------------------|
| RELEASE_PLAYBOOK.md existe | `esp32_wroom_32e/RELEASE_PLAYBOOK.md` (version, bump, build prod, manifest, test 1 device, rollout, DoD). |
| Version dans le code | `esp32_wroom_32e/src/app_config.h` L3 : `CURRENT_FIRMWARE_VERSION "1.0.21"`. |
| Manifest (version, url, size, sha256, sig) | Playbook §4 ; `ota.cpp` lit version, url, size, sha256, sig (manifest signé ECDSA), `OTA_PUBKEY_PEM` dans ota.cpp. |
| Scripts/outils réels | Pas de script unique “génération manifest” dans le repo ; playbook renvoie à dépôt/outil de distribution. Rollback et signature décrits dans l’audit. |

**Conclusion P0-3 :** Conforme. Playbook aligné avec le code (app_config.h, ota.cpp, RELEASE_PLAYBOOK.md).
