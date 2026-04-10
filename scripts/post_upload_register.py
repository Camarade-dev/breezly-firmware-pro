# scripts/post_upload_register.py
# Provisionne le device après upload via external_id dérivé du MAC (esptool read_mac).
# En mode flotte (BREEZLY_FLEET_FLASH=1), le script flash_fleet.py gère MAC + provision + EOL ;
# ce hook ne fait rien pour éviter doublon et surcharge en uploads parallèles.
# --variant STD|PREMIUM|B2B|B2B_PMS
env = None
try:
    Import("env")  # type: ignore[name-defined]
except NameError:
    pass
import os, sys, subprocess, json

def _resolve_script_dir():
    try:
        return os.path.dirname(os.path.abspath(__file__))
    except NameError:
        # En exécution SCons, __file__ peut être absent.
        if env is not None:
            try:
                return os.path.join(env.subst("$PROJECT_DIR"), "scripts")
            except Exception:
                pass
        return os.path.join(os.getcwd(), "scripts")

SCRIPT_DIR = _resolve_script_dir()
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

from provision_common import (
    external_id_from_mac,
    normalize_variant,
    parse_mac_from_text,
    read_external_id_from_serial,
)


def get_esptool_path():
    try:
        tool_dir = env.PioPlatform().get_package_dir("tool-esptoolpy")
        return os.path.join(tool_dir, "esptool.py") if tool_dir else None
    except Exception:
        return None

def get_upload_port():
    try:
        p = env.GetProjectOption("upload_port")
        if p and str(p).strip():
            return str(p).strip()
    except Exception:
        pass
    try:
        p = env.subst("$UPLOAD_PORT")
        if p and str(p).strip() and str(p).strip() != "$UPLOAD_PORT":
            return str(p).strip()
    except Exception:
        pass
    for k in ("UPLOAD_PORT", "PLATFORMIO_UPLOAD_PORT"):
        p = os.environ.get(k, "")
        if p and str(p).strip():
            return str(p).strip()
    return ""

def read_mac(port):
    """Lit le MAC via esptool."""
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
        mac = parse_mac_from_text(out)
        if mac:
            return mac.replace(":", "").upper()
        print(f"[post-upload] read_mac: format inattendu: {out[:200]!r}")
        return None
    except subprocess.CalledProcessError as e:
        out = getattr(e, "output", None) or getattr(e, "stdout", "") or str(e)
        print(f"[post-upload] read_mac fail (exit {e.returncode}): {out[:300]}")
        return None
    except Exception as e:
        print(f"[post-upload] read_mac fail: {e}")
        return None

def after_upload(target, source, env):
    if os.environ.get("BREEZLY_FLEET_FLASH") == "1":
        print("[post-upload] fleet mode: provisioning géré par flash_fleet.py")
        return
    print("[post-upload] hook loaded")
    port = get_upload_port()
    if port:
        print(f"[post-upload] port upload: {port}")
    else:
        print("[post-upload] aucun upload_port configuré")

    api_url = (
        env.GetProjectOption("custom_api_url")
        or os.environ.get("API_URL", "https://breezly-backendweb.onrender.com")
    )
    factory = env.GetProjectOption("custom_factory_token") or os.environ.get("FACTORY_TOKEN", "")
    devkey  = env.GetProjectOption("custom_device_key_b64") or os.environ.get("DEVICE_KEY_B64", "")

    # Variant du device : optionnelle.
    # 1) Si définie dans platformio.ini (custom_device_variant), on l'utilise.
    # 2) Sinon on lit DEVICE_VARIANT passée par flash_fleet.py ou l'environnement.
    try:
        variant = env.GetProjectOption("custom_device_variant")
    except Exception:
        variant = None
    if not variant:
        variant = os.environ.get("DEVICE_VARIANT", "")
    variant = normalize_variant(variant) or ""

    if not factory:
        raise RuntimeError("FACTORY token manquant (custom_factory_token / FACTORY_TOKEN)")
    if not devkey:
        raise RuntimeError("DEVICE_KEY_B64 manquante (custom_device_key_b64 / DEVICE_KEY_B64)")

    external_id, serial_diag = read_external_id_from_serial(port, timeout_sec=12)
    if external_id:
        print(f"[post-upload] external_id lu sur la serie ({port})")
    else:
        print(f"[post-upload] external_id serie indisponible: {serial_diag}")
        mac = read_mac(port)
        if not mac:
            raise RuntimeError("Impossible de lire l'external_id en serie ni le MAC via esptool")
        external_id = external_id_from_mac(mac)
        if not external_id:
            raise RuntimeError("Impossible de construire external_id a partir du MAC")
    print(f"[post-upload] external_id pour provision: {external_id}")

    payload = {
        "external_id": external_id,
        "deviceKeyB64": devkey,
        "name": external_id,
        "type": "temperature",
        "location": "Bureau"
    }
    if variant:
        payload["variant"] = variant

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
