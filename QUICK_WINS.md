# Quick Wins — < 1 journée

Actions à fort impact, réalisables en moins d’une journée de travail.

---

1. **Corriger la condition OTA périodique**  
   Remplacer `otaInProgress` par `otaIsInProgress()` dans `main.cpp` (condition du check OTA toutes les 12 h).  
   **Fichier:** `src/main.cpp` (déjà fait dans ce repo.)

2. **Unifier l’état OTA**  
   Supprimer l’export de `otaInProgress` dans `globals.h`/`globals.cpp` et toute utilisation restante; utiliser uniquement `otaIsInProgress()` (et `otaSetInProgress()`). Vérifier `sleep.h` (utilise encore `otaInProgress`).  
   **Fichiers:** `src/core/globals.h`, `globals.cpp`, `src/power/sleep.h`.

3. **MQTT: externaliser user/pass**  
   Créer un header généré en pre-build (ex. `src/net/mqtt_secrets.h`) avec `MQTT_USER` et `MQTT_PASS` lus depuis env ou `secrets.ini`; inclure ce fichier dans `mqtt_bus.cpp` et l’ajouter au .gitignore.  
   **Fichiers:** `mqtt_bus.cpp`, nouveau `mqtt_secrets.h` (généré), `scripts/pre_build_mqtt_secrets.py` (créer), `platformio.ini`.

4. **Secrets platformio: migrer vers secrets.ini**  
   Déplacer `custom_device_key_b64` et `custom_factory_token` de `platformio.ini` vers `secrets.ini` (créer le fichier, ajouter à .gitignore). Garder dans `platformio.ini` uniquement `extra_configs = secrets.ini`.  
   **Fichiers:** `secrets.ini` (template `secrets.ini.example` sans valeurs), `.gitignore`.

5. **Logger la cause de reset au boot**  
   Au début de `setup()`, appeler `esp_reset_reason()` et `esp_get_free_internal_heap_size()`, les afficher via `Serial.printf` (une ligne). Optionnel: stocker la dernière cause en NVS pour télémétrie.  
   **Fichier:** `src/main.cpp`.

6. **OTA: désactiver setInsecure pour GitHub**  
   Ajouter le certificat racine (ou intermédiaire) de `github.io` (ou de l’hébergeur utilisé) dans un PEM et l’utiliser pour la connexion HTTPS du manifest + .bin au lieu de `setInsecure()`.  
   **Fichier:** `src/ota/ota.cpp`; évent. `src/certs/` ou `ca_bundle.h`.

7. **Réduire les Serial.println(payload) en prod**  
   Remplacer les `Serial.println(s)` (payload JSON) par un log conditionnel sur un niveau (ex. `#if LOG_LEVEL >= 2` ou macro `LOG_VERBOSE`) pour ne pas afficher les données en build prod.  
   **Fichiers:** `src/main.cpp` (L412, 424 et al.), évent. macro dans `app_config.h`.

8. **Documenter APP_ENV_DEV vs BREEZLY_DEV**  
   `app_config.h` utilise `APP_ENV_DEV` pour l’URL du manifest OTA, alors que le build définit `BREEZLY_DEV` / `BREEZLY_PROD`. Soit définir `APP_ENV_DEV` quand `BREEZLY_DEV` est défini (dans platformio.ini: `-DAPP_ENV_DEV` pour l’env dev), soit utiliser `BREEZLY_DEV` dans `app_config.h` pour choisir l’URL.  
   **Fichiers:** `platformio.ini`, `src/app_config.h`.

9. **Backoff MQTT: premier pas**  
   Remplacer le délai fixe 8 s par un backoff simple: 8 s, 16 s, 32 s, plafond 120 s (et remise à 8 s après connexion réussie).  
   **Fichier:** `src/net/mqtt_bus.cpp`.

10. **Checklist factory exécutable**  
    Imprimer ou suivre [FACTORY_E2E_CHECKLIST.md](FACTORY_E2E_CHECKLIST.md) sur un appareil test; noter le temps réel et ajuster la checklist si besoin; valider que le post-upload register fonctionne avec l’API cible.  
    **Livrable:** Checklist mise à jour + note de temps EOL.
