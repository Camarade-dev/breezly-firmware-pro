# scripts/post_upload_register.py (seule la signature change)
Import("env")
import os, re, sys, subprocess

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

def read_mac(port: str | None) -> str | None:
    esptool_py = get_esptool_path()
    if not esptool_py:
        print("[post-upload] esptool.py introuvable via PlatformIO")
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

def build_name() -> str:
    mac = read_mac(get_upload_port())
    return f"PROV_{mac}" if mac else "PROV_FALLBACK"

# ✅ Signature correcte pour SCons: (target, source, env)
def after_upload(target, source, env):
    name      = os.environ.get("NAME") or build_name()
    sensor_ty = os.environ.get("TYPE", "temperature")
    location  = os.environ.get("LOCATION", "Bureau")

    api_url = os.environ.get("API_URL", "https://breezly-backend.onrender.com")
    token   = os.environ.get("TOKEN", "")

    child_env = os.environ.copy()
    child_env["API_URL"] = api_url
    child_env["TOKEN"]   = token

    cmd = [
        "node", "tools/decode-qrcode.cjs",
        f"--name={name}",
        f"--type={sensor_ty}",
        f"--location={location}",
    ]
    print(f"[post-upload] registering sensor: {' '.join(cmd)}")
    subprocess.check_call(cmd, env=child_env)

env.AddPostAction("upload", after_upload)
