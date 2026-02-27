# Plan de test — Control payload v1 (HMAC + anti-replay)

## 1. Prérequis

- Un device flashé en **dev** (`esp32-wroom-32e-dev`) pour accepter les commandes non signées.
- Un device (ou le même) flashé en **prod** pour exiger la signature.
- Backend avec au moins un capteur **provisionné** (`device_key_enc` en base).
- Token admin : `POST /api/admin/site/login` avec `{ "password": "<ADMIN_PASSWORD>" }` → récupérer `token`.

---

## 2. Tests backend (Node)

### 2.1 Signature HMAC (snapshot)

Vérifier que la chaîne canonical et la signature sont déterministes.

```bash
cd back-end-breezly
node -e "
const { buildCanonical, computeCtrlSigV1 } = require('./utils/ctrlSig');
const opts = { sensorId: 'TEST_ID', action: 'set_night_mode', ts: 1700000000, cmdId: 'abc123', args: { mode: 'on' } };
const canon = buildCanonical(opts);
const key = Buffer.alloc(32, 'x').toString('base64');
const sig = computeCtrlSigV1(opts, key);
console.log('canonical:', canon);
console.log('sig (first 20 chars):', sig.slice(0, 20));
"
```

À faire une fois et noter le `canonical` + `sig` pour vérifier que le firmware calcule la même chose (même canonical → même sig avec la même clé).

### 2.2 Endpoint admin control

Avec un vrai `sensorId` et le token admin :

```bash
# Remplacer <BACKEND>, <TOKEN>, <SENSOR_ID>
curl -s -X POST "https://<BACKEND>/api/admin/devices/<SENSOR_ID>/control" \
  -H "Authorization: Bearer <TOKEN>" \
  -H "Content-Type: application/json" \
  -d '{"action":"set_night_mode","mode":"on"}' | jq .
```

Attendu : `{ "ok": true, "sensorId": "...", "action": "set_night_mode" }`.  
Si 409 : capteur sans clé (non provisionné). Si 404 : mauvais `sensorId`.

---

## 3. Tests firmware (device en dev)

En **dev** le firmware accepte les commandes **sans** `sig` (CTRL_ALLOW_UNSIGNED=1). Tu peux envoyer depuis un client MQTT (ex. MQTTX) ou via le backend.

### 3.1 Commande non signée (dev uniquement)

Sur le topic `dev/breezly/devices/<sensorId>/control` (ou `prod/...` selon ton prefix) :

Payload :
```json
{"action":"set_night_mode","mode":"on"}
```

En dev : la commande doit être **acceptée** (ACK sur status). En prod : **rejet** (ack `ok: false`, reason `sig_missing` ou équivalent).

### 3.2 Commande signée (dev + prod)

Utiliser **uniquement** l’API admin (qui calcule `ts`, `cmdId`, `sig`) :

```bash
curl -s -X POST "https://<BACKEND>/api/admin/devices/<SENSOR_ID>/control" \
  -H "Authorization: Bearer <TOKEN>" \
  -H "Content-Type: application/json" \
  -d '{"action":"set_night_mode","mode":"off"}'
```

Vérifier sur le device (LED ou logs) que le mode nuit change, et sur le topic status un ACK `ack: set_night_mode`, `ok: true`.

### 3.3 Rejet signature invalide (prod)

En prod, envoyer un payload avec `sig` faux (ex. même payload que ci‑dessus mais en modifiant `sig` à la main via MQTT). Le device doit **rejeter** (ACK `ok: false`, reason `sig_invalid`).

### 3.4 Rejet timestamp (skew)

Envoyer une commande signée avec un `ts` très ancien (ex. `ts: 1600000000`). Le device doit rejeter (reason type `skew`).

### 3.5 Rejet replay (cmdId déjà vu)

Envoyer deux fois la **même** commande signée (même `ts`, même `cmdId`, même `sig`). La première doit être acceptée, la seconde **rejetée** (reason type `replay_id`).

### 3.6 Rate limit

Envoyer 4 commandes **signées** valides le plus vite possible (ex. 4 appels curl rapprochés). Les 2–3 premières peuvent passer, la 4ᵉ doit être **rejetée** (reason type `rate`).

### 3.7 factory_reset

- **Prod** : `{"action":"factory_reset","confirm":true,"holdMs":5000}` → doit être **rejeté** (factory_reset désactivé en prod par défaut).
- **Dev** : même payload → accepté seulement si `confirm: true` et `holdMs >= 5000` (sinon rejet `factory_confirm`).

### 3.8 set_wifi (optionnel)

- Envoyer `set_wifi` avec un `ssid` ou `password` trop long (ex. > 64 / 128 caractères) → rejet `set_wifi_len`.
- Pendant un OTA en cours, envoyer `set_wifi` → rejet `ota_busy` (si tu as implémenté le guard).

---

## 4. Récap rapide

| Test | Où | Attendu |
|------|-----|--------|
| Signature backend (canonical + sig) | Node | Pas d’erreur, canonical lisible |
| POST /admin/.../control | curl | 200, `ok: true` |
| Commande sans sig en dev | MQTT / curl | Acceptée |
| Commande sans sig en prod | MQTT | Rejet (sig_missing) |
| Commande signée (admin) | curl | Acceptée, ACK ok |
| Sig invalide en prod | MQTT (sig modifiée) | Rejet sig_invalid |
| ts trop vieux | MQTT | Rejet skew |
| Même cmdId deux fois | MQTT / curl | 2ᵉ rejet replay_id |
| 4 commandes rapides | curl x4 | Dernière rejet rate |
| factory_reset en prod | curl | Rejet factory_disabled |

---

## 5. Test unitaire backend (optionnel)

Créer `back-end-breezly/tests/ctrlSig.test.js` :

```javascript
const { buildCanonical, computeCtrlSigV1 } = require('../utils/ctrlSig');

describe('ctrlSig', () => {
  it('buildCanonical set_night_mode', () => {
    const c = buildCanonical({
      sensorId: 'S1',
      action: 'set_night_mode',
      ts: 1700000000,
      cmdId: 'id1',
      args: { mode: 'on' },
    });
    expect(c).toBe('v1|S1|set_night_mode|1700000000|id1|enabled=1');
  });

  it('computeCtrlSigV1 is deterministic', () => {
    const key = Buffer.alloc(32, 'a').toString('base64');
    const opts = { sensorId: 'S1', action: 'forget_wifi', ts: 1700000000, cmdId: 'x', args: {} };
    const s1 = computeCtrlSigV1(opts, key);
    const s2 = computeCtrlSigV1(opts, key);
    expect(s1).toBe(s2);
  });
});
```

Lancer : `npm test -- ctrlSig` (si Jest est configuré).
