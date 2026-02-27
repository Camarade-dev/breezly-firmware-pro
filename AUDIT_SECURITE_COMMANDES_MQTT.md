# Audit sécurité : commandes MQTT et objectif « device n’exécute que des commandes valides et destinées à lui »

**Objectif évalué :** *Même si quelqu’un connaît/observe tes topics, il ne peut pas déclencher n’importe quoi, et le device n’exécute que des commandes strictement valides, attendues, et destinées à lui.*

**Périmètre :** firmware `esp32_wroom_32e`, bus MQTT (HiveMQ Cloud).

---

## 1. Synthèse

| Critère | Conforme | Détail |
|--------|----------|--------|
| Commandes **strictement valides** (whitelist) | Oui | Seules 5 actions sont traitées ; le reste est ignoré. |
| Commandes **destinées à ce device** (topic device-scoped) | Oui | Topics `.../devices/{sensorId}/...` ; un device ne s’abonne qu’à ses propres topics. |
| **Pas d’exécution arbitraire** si on connaît les topics | Partiel | Dépend du broker : si un attaquant peut publier sur le topic (credentials, ACL), il peut envoyer des commandes valides. Pas de signature/authentification par commande côté firmware. |

**Verdict :** Le firmware respecte bien « commandes valides + destinées au device » dans son code. La garantie « quelqu’un qui observe ne peut pas déclencher » repose surtout sur la **sécurité du broker** (credentials, ACL, qui peut publier sur `/control`).

---

## 2. Ce qui est bien en place

### 2.1 Topics dédiés au device

- Base : `{prefix}/breezly/devices/{sensorId}` (ex. `prod/breezly/devices/ABC123`).
- Le device s’abonne uniquement à :
  - `.../devices/{sensorId}/control`
  - `.../devices/{sensorId}/ota`
  - `.../devices/{sensorId}/status` (pour « registered »).

Donc un observateur qui connaît le pattern mais pas le `sensorId` ne peut pas cibler ce device. Un device ne réagit qu’aux messages publiés sur **son** topic.

### 2.2 Whitelist d’actions

Dans `mqtt_bus.cpp`, `onMqttMessage()` pour `/control` :

- Le payload doit être du JSON avec un champ `action`.
- Seules ces actions sont exécutées :
  - `set_wifi`
  - `update` (OTA)
  - `set_night_mode`
  - `forget_wifi`
  - `factory_reset`
- Toute autre valeur d’`action` (ou chaîne vide) conduit à un `return` sans effet → aucune commande arbitraire exécutée.

### 2.3 Validation JSON et champs

- Parse JSON ; en cas d’erreur, le message est ignoré.
- Pour `set_wifi` : validation des champs (ssid, password, EAP, etc.) dans `applyWifiPrefsFromJson`.
- Pour `forget_wifi` : usage de `cmdId` / `ts` pour éviter les doublons (idempotence).

### 2.4 Connexion MQTT

- TLS (port 8883), CA bundle, credentials (user/pass) dans `mqtt_secrets.h` (build-time). Seul un client ayant ces identifiants peut se connecter au broker.

---

## 3. Points de vigilance / risques

### 3.1 Pas d’authentification par commande

- Aucune signature HMAC, token, ou nonce côté payload.
- Toute personne qui peut **publier** sur `.../devices/{sensorId}/control` (par ex. même compte MQTT que le device, ou ACL trop permissives) peut envoyer `{"action":"factory_reset"}`, `update`, etc.
- **Mitigation :** côté broker (HiveMQ Cloud), restreindre les ACL pour que seul le back-end (ou un compte dédié) puisse publier sur `.../control` et `.../ota` ; le device ne fait que s’abonner et publier status/telemetry.

### 3.2 Replay sur certaines actions

- `forget_wifi` : partiellement protégé par `cmdId` / `ts` (idempotence).
- `factory_reset` et `update` : pas de mécanisme anti-replay visible. Un message retained ou rejoué peut refaire l’action.
- **Recommandation :** ajouter un `ts` ou `cmdId` pour les actions critiques et les ignorer si trop ancien ou déjà traité.

### 3.3 Topic OTA

- Le device s’abonne à `.../ota` mais le handler est vide (commentaire « si tu veux des commandes OTA brutes ici »). Aucune exécution actuelle → pas de risque supplémentaire pour l’instant. Si tu ajoutes un traitement OTA par ce topic, appliquer la même logique (whitelist, validation, et si possible auth/signature).

### 3.4 Secret MQTT et `sensorId`

- `MQTT_USER` / `MQTT_PASS` : générés en build, pas commités (gitignore). Bonne pratique.
- `sensorId` : identifiant du device ; s’il fuit (logs, telemetry, autre), un attaquant peut connaître le topic cible. La sécurité repose alors entièrement sur le fait que lui ne peut pas publier (ACL broker).

---

## 4. Recommandations

1. **Broker :** Vérifier les ACL HiveMQ Cloud pour que seuls les comptes « backend » ou « app » puissent publier sur `.../control` et `.../ota` ; les devices uniquement sur leurs topics de status/telemetry et abonnement à leur propre `/control` et `/ota`.
2. **Anti-replay :** Pour `factory_reset` et `update`, exiger un `ts` (ou `cmdId`) et ignorer les commandes trop anciennes ou déjà appliquées.
3. **Optionnel (renforcement) :** Pour les commandes sensibles, signer les payloads côté backend (HMAC avec une clé par device ou partagée) et vérifier la signature dans le firmware avant d’exécuter.

---

## 5. Fichiers audités

- `src/net/mqtt_bus.cpp` (callback `onMqttMessage`, topics, subscriptions, handlers)
- `src/net/mqtt_bus.h` (API topics)
- `src/core/globals.h` (sensorId, prefs)
- Référence : `src/core/devkey.h` (présent mais non utilisé dans le flux MQTT des commandes)

---

*Rapport généré pour le firmware esp32_wroom_32e – objectif « commandes strictement valides et destinées au device ».*
