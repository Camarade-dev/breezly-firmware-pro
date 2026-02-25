Import("env")
import os, pathlib, sys

DEVKEY_PATH = pathlib.Path("src/core/devkey.h")

# 1) option PIO pour l'env courant, 2) option [env] (commun), 3) variable d'environnement
key_b64 = None
try:
    key_b64 = env.GetProjectOption("custom_device_key_b64")
except Exception:
    pass
if not key_b64:
    try:
        key_b64 = env.GetProjectConfig().get("env", "custom_device_key_b64")
    except Exception:
        pass
if not key_b64:
    key_b64 = os.environ.get("DEVICE_KEY_B64")

# en PROD, impose la présence
if not key_b64:
    print("[pre-build][FATAL] DEVICE KEY manquante (custom_device_key_b64 ou env DEVICE_KEY_B64)")
    sys.exit(1)

DEVKEY_PATH.parent.mkdir(parents=True, exist_ok=True)
DEVKEY_PATH.write_text(f'#pragma once\n#define DEVICE_KEY_B64 "{key_b64}"\n', encoding="utf-8")
print(f"[pre-build] wrote {DEVKEY_PATH} (len={len(key_b64)})")
