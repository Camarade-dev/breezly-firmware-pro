"""
Pre-build: génère src/net/mqtt_secrets.h à partir de secrets.ini ou variables d'environnement.
Les identifiants MQTT ne doivent jamais être en dur dans le code versionné.
"""
Import("env")
import os
import pathlib
import sys

OUTPUT_PATH = pathlib.Path("src/net/mqtt_secrets.h")

# 1) Option PIO (secrets.ini [env] custom_mqtt_user / custom_mqtt_pass), 2) env
user = env.GetProjectOption("custom_mqtt_user", None) or os.environ.get("MQTT_USER")
password = env.GetProjectOption("custom_mqtt_pass", None) or os.environ.get("MQTT_PASS")

if not user or not password:
    print("[pre-build][FATAL] MQTT credentials manquantes: custom_mqtt_user/custom_mqtt_pass dans secrets.ini ou MQTT_USER/MQTT_PASS en env")
    sys.exit(1)

def escape_c(s):
    return s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\r", "\\r")

user_esc = escape_c(user)
pass_esc = escape_c(password)

OUTPUT_PATH.parent.mkdir(parents=True, exist_ok=True)
content = f'''#pragma once
/* Généré par pre_build_mqtt_secrets.py - ne pas committer (gitignore) */
#define MQTT_SECRET_USER "{user_esc}"
#define MQTT_SECRET_PASS "{pass_esc}"
'''
OUTPUT_PATH.write_text(content, encoding="utf-8")
print(f"[pre-build] wrote {OUTPUT_PATH} (user len={len(user)})")
