# scripts/post_upload_register.py


# Provisionne le device après upload. Préfère l'external_id lu sur la série (envoyé par le
# firmware au boot) pour garantir la cohérence avec le nom BLE (téléversement parallèle, etc.).

Import("env")
import os, re, sys, subprocess, json, time

SERIAL_BAUD = 115200
BOOT_WAIT_S = 5
SERIAL_READ_TIMEOUT_S = 18
MARKER = "BREEZLY_EXTERNAL_ID="
ALLOW_MAC_FALLBACK = os.environ.get("ALLOW_MAC_FALLBACK", "0").strip() == "1"

def get_esptool_path():
    try:
        tool_dir = env.PioPlatform().get_package_dir("tool-esptoolpy")
        return os.path.join(tool_dir, "esptool.py") if tool_dir else None
    except Exception:
        return None

def get_upload_port():
    try:
        return env.GetProjectOption("upload_port")
    except Exception:
        return os.environ.get("UPLOAD_PORT", "")

def read_mac(port):
    esptool_py = get_esptool_path()
    if not esptool_py:
        print("[post-upload] esptool.py introuvable")
        return None
    cmd = [sys.executable, esptool_py]
    if port:
        cmd += ["--port", port]
    cmd += ["read_mac"]
    try:
        out = subprocess.check_output(cmd, stderr=subprocess.STDOUT, text=True)
        m = re.search(r"MAC:\s*([0-9A-Fa-f:]{17})", out)
        return m.group(1).replace(":", "").upper() if m else None
    except Exception as e:
        print(f"[post-upload] read_mac fail: {e}")
        return None

def external_id_from_serial(port):
    """Lit l'external_id envoyé par le device sur la série au boot (même valeur que le nom BLE)."""
    if not port or not str(port).strip():
        return None
    try:
        import serial
    except ImportError:
        print("[post-upload] pyserial non disponible, fallback esptool read_mac")
        return None
    try:
        ser = serial.Serial(port, SERIAL_BAUD, timeout=0.5)
    except Exception as e:
        print(f"[post-upload] ouverture série {port} impossible: {e}")
        return None
    try:
        deadline = time.monotonic() + SERIAL_READ_TIMEOUT_S
        buf = ""
        while time.monotonic() < deadline:
            chunk = ser.read(256)
            if chunk:
                buf += chunk.decode("utf-8", errors="ignore")
            while "\n" in buf or "\r" in buf:
                line, _, buf = buf.partition("\n")
                if "\r" in line:
                    line = line.split("\r")[0]
                line = line.strip()
                if line.startswith(MARKER):
                    val = line[len(MARKER):].strip()
                    if val.startswith("PROV_") and len(val) == 17:
                        return val
            time.sleep(0.05)
        return None
    finally:
        try:
            ser.close()
        except Exception:
            pass

# IMPORTANT: parameter must be named 'env'
# IMPORTANT: parameter must be named 'env'
def after_upload(target, source, env):
    print("[post-upload] hook loaded")
    port = get_upload_port()
    if port:
        print(f"[post-upload] port upload: {port}")
    else:
        print("[post-upload] aucun upload_port configuré")

    # lit d'abord l’option PlatformIO, sinon API_URL, sinon défaut DEV
    api_url = (
        env.GetProjectOption("custom_api_url")
        or os.environ.get("API_URL", "https://breezly-backendweb.onrender.com")
    )
    factory = env.GetProjectOption("custom_factory_token") or os.environ.get("FACTORY_TOKEN", "")
    devkey  = env.GetProjectOption("custom_device_key_b64") or os.environ.get("DEVICE_KEY_B64", "")

    if not factory:
        raise RuntimeError("FACTORY token manquant (custom_factory_token / FACTORY_TOKEN)")
    if not devkey:
        raise RuntimeError("DEVICE_KEY_B64 manquante (custom_device_key_b64 / DEVICE_KEY_B64)")

    # 1) Attendre le boot puis lire l'external_id sur la série (identique au nom BLE)
    print(f"[post-upload] attente boot {BOOT_WAIT_S}s puis lecture série...")
    time.sleep(BOOT_WAIT_S)
    external_id = external_id_from_serial(port)

    # 2) Fallback optionnel: esptool read_mac.
    # Désactivé par défaut pour éviter de provisionner un mauvais external_id
    # (ex: série indisponible -> mismatch DB vs nom BLE réel).
    if not external_id:
        if not ALLOW_MAC_FALLBACK:
            raise RuntimeError(
                "Impossible de lire BREEZLY_EXTERNAL_ID sur série. "
                "Provisioning interrompu pour éviter un external_id incohérent. "
                "Installe pyserial / vérifie le port série, ou exporte ALLOW_MAC_FALLBACK=1 pour forcer le fallback."
            )
        print("[post-upload] pas de BREEZLY_EXTERNAL_ID sur série, fallback esptool read_mac (ALLOW_MAC_FALLBACK=1)")
        mac = read_mac(port)
        if not mac:
            raise RuntimeError("Impossible de lire le MAC (esptool)")
        external_id = f"PROV_{mac[:12]}"

    print(f"[post-upload] external_id pour provision: {external_id}")

    payload = {
        "external_id": external_id,
        "deviceKeyB64": devkey,
        "name": external_id,
        "type": "temperature",
        "location": "Bureau"
    }

    cmd = [
        sys.executable, "-c",
        (
          "import sys,os,json,urllib.request;"
          "url=os.environ['API_URL']+'/api/internal/provision-device';"
          "req=urllib.request.Request(url, data=json.dumps(json.loads(sys.argv[1])).encode(), "
          "headers={'Content-Type':'application/json','X-Factory-Token':os.environ.get('FACTORY_TOKEN','')});"
          "print(urllib.request.urlopen(req).read().decode())"
        ),
        json.dumps(payload)
    ]
    env_env = os.environ.copy()
    env_env["API_URL"] = api_url
    env_env["FACTORY_TOKEN"] = factory
    subprocess.check_call(cmd, env=env_env)


env.AddPostAction("upload", after_upload)
