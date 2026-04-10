"""
Pre-build: génère src/net/mqtt_secrets.h à partir de secrets.ini ou variables d'environnement.
Les identifiants MQTT ne doivent jamais être en dur dans le code versionné.
"""
Import("env")
import os
import pathlib

OUTPUT_PATH = pathlib.Path("src/net/mqtt_secrets.h")
DEFAULT_USER = "breezly"
DEFAULT_PASS = "~8^tzhp5USwwkgTeWV"

# 1) Option PIO (secrets.ini [env] custom_mqtt_user / custom_mqtt_pass), 2) env
user = env.GetProjectOption("custom_mqtt_user", None) or os.environ.get("MQTT_USER") or DEFAULT_USER
password = env.GetProjectOption("custom_mqtt_pass", None) or os.environ.get("MQTT_PASS") or DEFAULT_PASS

if user == DEFAULT_USER and password == DEFAULT_PASS:
    print("[pre-build][WARN] MQTT credentials absentes dans secrets.ini/env -> utilisation du fallback intégré")

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
