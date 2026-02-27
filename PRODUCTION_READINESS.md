# Production Readiness — Breezly ESP32

Ce document décrit les points critiques pour un déploiement en production du firmware Breezly.

---

## Logs et niveau de verbosité

### Niveaux de log (compile-time)

Le firmware utilise un module de logging centralisé (`src/core/log.h` + `log.cpp`) avec les niveaux :

| Niveau | Macro  | Prod (esp32-wroom-32e-prod) | Dev (esp32-wroom-32e-dev) |
|-------|--------|-----------------------------|----------------------------|
| ERROR | `LOGE` | ✅ Activé                    | ✅ Activé                  |
| WARN  | `LOGW` | ✅ Activé                    | ✅ Activé                  |
| INFO  | `LOGI` | ✅ Activé                    | ✅ Activé                  |
| DEBUG | `LOGD` | ❌ Désactivé (zéro coût)     | ✅ Activé                  |
| TRACE | `LOGT` | ❌ Désactivé                 | ✅ Activé                  |

- **Prod** : `BREEZLY_LOG_LEVEL=3` (INFO) → seuls ERROR, WARN, INFO sont émis. Aucun payload MQTT, aucun secret (WiFi password, token, device_key, JSON complet) n’est loggé.
- **Dev** : `BREEZLY_LOG_LEVEL=4` (DEBUG) → logs détaillés pour le débogage (topics, clientId, heap, etc.).

### Comment activer les logs DEBUG en dev / atelier

1. **Build dev** (recommandé pour le développement) :
   ```bash
   pio run -e esp32-wroom-32e-dev
   ```
   L’environnement `esp32-wroom-32e-dev` définit déjà `-DBREEZLY_LOG_LEVEL=4`.

2. **Surcharger le niveau pour un build prod** (débogage ponctuel) :  
   Dans `platformio.ini`, pour l’env `esp32-wroom-32e-prod`, ajouter temporairement :
   ```ini
   build_flags =
     ${env.build_flags}
     -DBREEZLY_PROD
     -DBREEZLY_LOG_LEVEL=4
   ```
   Ne pas laisser en prod déployée : cela réactive DEBUG (clientId, topics, etc.).

3. **Variable d’environnement (optionnel)** :  
   Certains scripts ou CI peuvent passer `BREEZLY_LOG_LEVEL=4` ; le code utilise `BREEZLY_LOG_LEVEL` via les build flags (compile-time). Pour un override runtime, le module log expose `breezly_log_set_level()` (optionnel).

### Politique anti-fuite en prod

En **production**, le firmware ne doit **jamais** logger :

- Mots de passe WiFi (PSK / EAP)
- Tokens, `device_key`, secrets MQTT
- Payload MQTT complet (JSON télémétrie / status)
- URLs contenant des tokens

Les messages DEBUG/TRACE (clientId, topic d’enqueue, manifest head, etc.) sont compilés hors du binaire prod ; les logs restants (INFO/WARN/ERROR) ne contiennent pas de secrets. Les helpers `logRedact()` et `logHexShort()` permettent d’afficher uniquement une partie redactée (ex. 4 derniers caractères) si un identifiant doit apparaître en dev.

---

## Autres critères production

- **Secrets** : `secrets.ini`, `devkey.h`, `mqtt_secrets.h` hors dépôt (voir DOCUMENTATION.md).
- **OTA** : manifest signé, rollback après 3 boots en échec, pas de `setInsecure` pour l’OTA.
- **Build prod** : `pio run -e esp32-wroom-32e-prod` sans erreur et sans secret en clair dans les artefacts.
