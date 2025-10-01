# scripts/post_upload_register.py


Import("env")
import os, re, sys, subprocess, json

def reverse_pairs(mac_hex):
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

# IMPORTANT: parameter must be named 'env'
def after_upload(target, source, env):
    print("[post-upload] hook loaded")

    # read from PlatformIO options first, then env vars
    api_url = env.GetProjectOption("custom_api_url") or os.environ.get("API_URL", "https://breezly-backend.onrender.com")
    factory = env.GetProjectOption("custom_factory_token") or os.environ.get("FACTORY_TOKEN", "")
    devkey  = env.GetProjectOption("custom_device_key_b64") or os.environ.get("DEVICE_KEY_B64", "")

    if not factory:
        raise RuntimeError("FACTORY token manquant (custom_factory_token / FACTORY_TOKEN)")
    if not devkey:
        raise RuntimeError("DEVICE_KEY_B64 manquante (custom_device_key_b64 / DEVICE_KEY_B64)")

    mac = read_mac(get_upload_port())
    if not mac:
        raise RuntimeError("Impossible de lire le MAC")
    external_id = f"PROV_{reverse_pairs(mac)}"

    payload = {
        "external_Id": external_id,
        "deviceKeyB64": devkey,
        "name": external_id,
        "type": "temperature",
        "location": "Bureau"
    }

    # minimal HTTP POST using stdlib
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
