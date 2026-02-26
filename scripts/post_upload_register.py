# scripts/post_upload_register.py
# Provisionne le device après upload via external_id dérivé du MAC (esptool read_mac).

Import("env")
import os, re, sys, subprocess, json

def reverse_pairs(mac_hex):
    """Inverse l'ordre des paires d'octets pour coller au format buildExternalId() du firmware."""
    mac_hex = mac_hex.replace(":", "").upper()
    return "".join([mac_hex[i:i+2] for i in range(0, 12, 2)][::-1])

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
        m = re.search(r"MAC:\s*([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})", out)
        if m:
            return m.group(1).replace(":", "").upper()
        m = re.search(r"([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})", out)
        if m:
            return m.group(1).replace(":", "").upper()
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

    if not factory:
        raise RuntimeError("FACTORY token manquant (custom_factory_token / FACTORY_TOKEN)")
    if not devkey:
        raise RuntimeError("DEVICE_KEY_B64 manquante (custom_device_key_b64 / DEVICE_KEY_B64)")

    mac = read_mac(port)
    if not mac:
        raise RuntimeError("Impossible de lire le MAC (esptool)")
    external_id = f"PROV_{reverse_pairs(mac)}"
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
