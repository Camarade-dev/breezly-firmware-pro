# Control payload v1 — schéma, canonical, config, exemples

## 1. Format payload (compat)

Le device s’abonne à `{prefix}/breezly/devices/{sensorId}/control`. Le payload JSON doit contenir au minimum :

```json
{
  "action": "set_wifi|update|set_night_mode|forget_wifi|factory_reset",
  "ts": 1700000000,
  "cmdId": "base64_or_hex_short",
  "sig": "base64(hmac_sha256(canonical))",
  "...": "champs spécifiques à l’action"
}
```

- **ts** : epoch secondes (obligatoire en prod).
- **cmdId** : 8–16 bytes aléatoire, hex ou base64 (obligatoire en prod).
- **sig** : HMAC-SHA256 de la chaîne canonical, encodé en base64 (obligatoire en prod).
- En **dev** (`CTRL_ALLOW_UNSIGNED=1`), les commandes sans `ts`/`cmdId`/`sig` peuvent être acceptées.

## 2. Chaîne canonical (firmware + backend)

Format exact (à faire coïncider entre firmware et backend) :

```
v1|{sensorId}|{action}|{ts}|{cmdId}|{actionArgsCanonical}
```

### actionArgsCanonical par action

| Action          | actionArgsCanonical |
|-----------------|---------------------|
| set_wifi        | `ssid=<ssid>\|eap=<0/1>\|user=<username_or_empty>\|pw=<password_or_empty>` |
| update          | `target=<prod/dev>\|force=<0/1>` |
| set_night_mode  | `enabled=<0/1>` (on=1, off/auto=0) |
| forget_wifi     | `` (vide) |
| factory_reset   | `confirm=<0/1>\|holdMs=<int>` |

Exemple canonical pour `set_night_mode` (mode=on, ts=1700000000, cmdId=abc123) :

```
v1|PROV_88D9C6215788|set_night_mode|1700000000|abc123|enabled=1
```

## 3. HMAC

- **Algorithme** : HMAC-SHA256(canonical, deviceKey).
- **Clé** : clé device (32 bytes), en base64 dans NVS / `devkey.h` (firmware) ou `device_key_enc` décrypté (backend).
- **Signature** : 32 bytes HMAC encodés en base64, placés dans le champ `sig`.
- Comparaison côté firmware : **constant-time**.

## 4. Config firmware (app_config.h)

| Macro                         | Prod   | Dev    | Description |
|------------------------------|--------|--------|-------------|
| CTRL_REQUIRE_SIG             | 1      | 0      | Exiger ts, cmdId, sig |
| CTRL_ALLOW_UNSIGNED          | 0      | 1      | Accepter commandes non signées (debug) |
| CTRL_FACTORY_RESET_ENABLED   | 0      | 1      | Autoriser action factory_reset |
| CTRL_MAX_SKEW_SEC            | 300    | 300    | Fenêtre de validité du ts (secondes) |
| CTRL_CMDID_RING_SIZE        | 16     | 16     | Taille du ring buffer anti-replay |
| CTRL_RATE_LIMIT_MIN_MS      | 2000   | 2000   | 1 commande / 2 s (refill) |
| CTRL_RATE_LIMIT_BURST       | 3      | 3      | Nombre max de tokens |
| CTRL_FACTORY_RESET_REQUIRE_HOLD_MS | 5000 | 5000 | holdMs minimum pour factory_reset |
| CTRL_PAYLOAD_MAX_BYTES      | 1024   | 1024   | Taille max du payload JSON |
| CTRL_SET_WIFI_SSID_MAX      | 64     | 64     | Longueur max SSID |
| CTRL_SET_WIFI_PASSWORD_MAX  | 128    | 128    | Longueur max mot de passe |

## 5. Exemples curl (backend)

Base URL : `https://<backend>/api`  
Auth : `Authorization: Bearer <admin_token>` (obtenu via `POST /api/admin/site/login` avec `password`).

### set_night_mode (mode nuit)

```bash
curl -s -X POST "https://<backend>/api/admin/devices/<sensorId>/control" \
  -H "Authorization: Bearer <admin_token>" \
  -H "Content-Type: application/json" \
  -d '{"action":"set_night_mode","mode":"on"}'
```

### update (déclencher check OTA)

```bash
curl -s -X POST "https://<backend>/api/admin/devices/<sensorId>/control" \
  -H "Authorization: Bearer <admin_token>" \
  -H "Content-Type: application/json" \
  -d '{"action":"update","target":"prod","force":0}'
```

### forget_wifi

```bash
curl -s -X POST "https://<backend>/api/admin/devices/<sensorId>/control" \
  -H "Authorization: Bearer <admin_token>" \
  -H "Content-Type: application/json" \
  -d '{"action":"forget_wifi"}'
```

### factory_reset (si activé en build)

```bash
curl -s -X POST "https://<backend>/api/admin/devices/<sensorId>/control" \
  -H "Authorization: Bearer <admin_token>" \
  -H "Content-Type: application/json" \
  -d '{"action":"factory_reset","confirm":true,"holdMs":5000}'
```

### set_wifi (changement WiFi)

```bash
curl -s -X POST "https://<backend>/api/admin/devices/<sensorId>/control" \
  -H "Authorization: Bearer <admin_token>" \
  -H "Content-Type: application/json" \
  -d '{"action":"set_wifi","ssid":"MySSID","password":"secret","authType":"psk"}'
```

## 6. Fichiers modifiés / créés

### Firmware (esp32_wroom_32e)

- **Créés** : `src/net/mqtt_ctrl.h`, `src/net/mqtt_ctrl.cpp`
- **Modifiés** : `src/app_config.h` (macros CTRL_*), `src/net/mqtt_bus.h` (API control), `src/net/mqtt_bus.cpp` (délégation vers ctrl + handlers exportés)

### Backend (back-end-breezly)

- **Créé** : `utils/ctrlSig.js` (buildCanonical, computeCtrlSigV1, buildSignedControlPayload)
- **Modifié** : `mqtt/mqttBus.js` (publishControl, sendForgetWifi signé si clé dispo), `routes/adminSite.js` (POST /admin/devices/:sensorId/control)

## 7. Anti-replay et garde-fous

- **ts** : `abs(now - ts) <= CTRL_MAX_SKEW_SEC`.
- **cmdId** : ring buffer en RAM des derniers cmdId acceptés ; rejet si déjà vu.
- **Rate limit** : token bucket (1 cmd / 2 s, burst 3).
- **factory_reset** : en prod désactivé par défaut ; si activé, exige `confirm=true` et `holdMs >= 5000`.
- **set_wifi** : longueurs max SSID/password ; refus si OTA en cours.
