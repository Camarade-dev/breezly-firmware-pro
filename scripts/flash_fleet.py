#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Flash en parallèle une flotte d'ESP32 (hub USB multi-ports) et remplit automatiquement
le journal EOL (traçabilité commerciale). Orchestration explicite : build → flash → MAC → provision → EOL.

Architecture robuste : pas de dépendance au parsing stdout pour les infos critiques ;
timeouts, retries et résultat structuré par port.
"""

import argparse
import configparser
import csv
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import List, Optional, Tuple

DEFAULT_PIO_EXE = r"C:\Users\stris\.platformio\penv\Scripts\platformio.exe"
EOL_LOG_DEFAULT = "docs/EOL_LOG.csv"
APP_CONFIG_PATH = "src/app_config.h"
SECRETS_INI = "secrets.ini"

# Timeouts (secondes)
UPLOAD_TIMEOUT_SEC = 120
MAC_READ_TIMEOUT_SEC = 15
PROVISION_TIMEOUT_SEC = 30

# Retries par étape
UPLOAD_RETRIES = 2
MAC_READ_RETRIES = 3
PROVISION_RETRIES = 2

# Délai après upload avant lecture MAC (stabilisation port COM)
POST_UPLOAD_DELAY_SEC = 2

# Jobs parallèles par défaut (conservateur pour Windows / CP210x)
DEFAULT_JOBS = 2

# URL backend par défaut (prod)
DEFAULT_API_URL = "https://breezly-backend.onrender.com"


def now() -> str:
    return datetime.now().strftime("%H:%M:%S")


# ---------------------------------------------------------------------------
# Résultat structuré par port
# ---------------------------------------------------------------------------

@dataclass
class PortResult:
    port: str
    flash_ok: bool
    flash_attempts: int
    mac: Optional[str] = None
    external_id: Optional[str] = None
    provision_ok: bool = False
    fw_version: str = ""
    variant: str = ""
    error_stage: Optional[str] = None
    remarks: str = ""

    def to_dict(self):
        return {
            "port": self.port,
            "flash_ok": self.flash_ok,
            "flash_attempts": self.flash_attempts,
            "mac": self.mac,
            "external_id": self.external_id,
            "provision_ok": self.provision_ok,
            "fw_version": self.fw_version,
            "variant": self.variant,
            "error_stage": self.error_stage,
            "remarks": self.remarks,
        }


# ---------------------------------------------------------------------------
# Version firmware
# ---------------------------------------------------------------------------

def get_fw_version(cwd: str) -> str:
    """Lit CURRENT_FIRMWARE_VERSION depuis src/app_config.h."""
    path = Path(cwd) / APP_CONFIG_PATH
    if not path.exists():
        return "unknown"
    try:
        text = path.read_text(encoding="utf-8", errors="ignore")
        m = re.search(r'#define\s+CURRENT_FIRMWARE_VERSION\s+"([^"]+)"', text)
        return m.group(1) if m else "unknown"
    except Exception:
        return "unknown"


# ---------------------------------------------------------------------------
# Secrets (secrets.ini + env)
# ---------------------------------------------------------------------------

def load_secrets(cwd: str) -> Tuple[str, str]:
    """
    Charge factory_token et device_key_b64 depuis secrets.ini [env] ou variables d'environnement.
    Retourne (factory_token, device_key_b64).
    """
    factory_token = os.environ.get("FACTORY_TOKEN", "")
    device_key_b64 = os.environ.get("DEVICE_KEY_B64", "")

    secrets_path = Path(cwd) / SECRETS_INI
    if secrets_path.exists():
        try:
            cfg = configparser.ConfigParser()
            cfg.read(secrets_path, encoding="utf-8")
            if cfg.has_section("env"):
                factory_token = cfg.get("env", "custom_factory_token", fallback=factory_token).strip()
                device_key_b64 = cfg.get("env", "custom_device_key_b64", fallback=device_key_b64).strip()
        except Exception:
            pass

    return factory_token, device_key_b64


# ---------------------------------------------------------------------------
# EOL log
# ---------------------------------------------------------------------------

EOL_HEADER = ["Date", "Heure", "MAC", "external_id", "Version_FW", "Variant", "Port", "EOL_resultat", "Operateur", "Remarques"]


def ensure_eol_log_header(eol_log_path: Path) -> None:
    """Crée le fichier EOL avec en-tête CSV si absent ; migre l'ancien format vers le nouveau."""
    eol_log_path = Path(eol_log_path)
    eol_log_path.parent.mkdir(parents=True, exist_ok=True)
    if eol_log_path.exists():
        with open(eol_log_path, "r", newline="", encoding="utf-8") as f:
            all_rows = list(csv.reader(f))
        if all_rows and "Variant" in all_rows[0]:
            return
        with open(eol_log_path, "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            w.writerow(EOL_HEADER)
            for r in all_rows[1:]:
                if len(r) >= 8:
                    w.writerow([r[0], r[1], r[2], r[3], r[4], "", "", r[5], r[6], r[7]])
                elif len(r) >= 6:
                    w.writerow([r[0], r[1], r[2], r[3], r[4], "", "", r[5], r[6] if len(r) > 6 else "", ""])
        return
    with open(eol_log_path, "w", newline="", encoding="utf-8") as f:
        csv.writer(f).writerow(EOL_HEADER)


def append_eol_row(
    eol_log_path: Path,
    date: str,
    time_str: str,
    mac: str,
    external_id: str,
    version_fw: str,
    variant: str,
    port: str,
    result: str,
    operator: str,
    remarks: str,
) -> None:
    """Ajoute une ligne au journal EOL."""
    path = Path(eol_log_path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "a", newline="", encoding="utf-8") as f:
        csv.writer(f).writerow([
            date, time_str, mac or "", external_id or "", version_fw, variant or "", port or "",
            result, operator or "", remarks or "",
        ])


def eol_result_and_remarks(res: PortResult) -> Tuple[str, str]:
    """Dérive EOL_resultat et Remarques à partir du résultat structuré (pas de parsing stdout)."""
    if not res.flash_ok:
        return "KO", "upload_failed"
    if res.mac is None or res.external_id is None:
        return "KO", "mac_read_failed"
    if res.remarks == "provision_skipped_no_secrets":
        return "OK", res.remarks
    if not res.provision_ok:
        return "KO", res.remarks or "provision_failed"
    return "OK", res.remarks or ""


# ---------------------------------------------------------------------------
# Subprocess avec timeout
# ---------------------------------------------------------------------------

def run_cmd(
    cmd: list,
    cwd: Optional[str] = None,
    env: Optional[dict] = None,
    capture: bool = True,
    timeout_sec: Optional[int] = None,
) -> Tuple[int, str, str]:
    """Exécute une commande. Retourne (returncode, stdout, stderr)."""
    kw = dict(cwd=cwd, env=env, shell=False)
    if timeout_sec is not None:
        kw["timeout"] = timeout_sec
    if capture:
        p = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            **kw,
        )
        return p.returncode, p.stdout or "", p.stderr or ""
    p = subprocess.run(cmd, **kw)
    return (p.returncode, "", "")


# ---------------------------------------------------------------------------
# PlatformIO
# ---------------------------------------------------------------------------

def ensure_platformio(pio_exe: str) -> None:
    if not Path(pio_exe).exists():
        raise FileNotFoundError(
            f"platformio.exe introuvable: {pio_exe}\n"
            "→ Mets le bon chemin via --pio-exe"
        )


def list_ports_json(pio_exe: str, cwd: str) -> list:
    cmd = [pio_exe, "device", "list", "--json-output"]
    code, out, err = run_cmd(cmd, cwd=cwd, capture=True, timeout_sec=30)
    if code != 0:
        raise RuntimeError(f"[device list] échec ({code})\n{err.strip()}")
    try:
        return json.loads(out)
    except json.JSONDecodeError as e:
        raise RuntimeError(f"[device list] JSON invalide: {e}\nSortie:\n{out}")


def normalize_port(p: Optional[str]) -> str:
    return (p or "").strip()


def pick_ports(
    devices: list,
    include: Optional[list] = None,
    exclude: Optional[list] = None,
) -> list:
    """Filtre les ports selon include/exclude sur description, hwid, port."""
    include = include or []
    exclude = exclude or []
    ports = []
    for d in devices:
        port = normalize_port(d.get("port"))
        desc = (d.get("description") or "")
        hwid = (d.get("hwid") or "")
        hay = f"{port} {desc} {hwid}".lower()
        if include and not any(s.lower() in hay for s in include):
            continue
        if exclude and any(s.lower() in hay for s in exclude):
            continue
        if port:
            ports.append(port)
    seen = set()
    return [p for p in ports if p not in seen and not seen.add(p)]


def pio_build(pio_exe: str, env_name: str, cwd: str) -> None:
    cmd = [pio_exe, "run", "--environment", env_name]
    print(f"[{now()}] Build: {env_name}")
    code, out, err = run_cmd(cmd, cwd=cwd, capture=True, timeout_sec=300)
    if out.strip():
        print(out.rstrip())
    if code != 0:
        if err.strip():
            print(err.rstrip(), file=sys.stderr)
        raise RuntimeError(f"Build échoué ({code})")
    print(f"[{now()}] Build OK")


def pio_upload_one(
    pio_exe: str,
    env_name: str,
    port: str,
    cwd: str,
    variant: Optional[str],
    timeout_sec: int = UPLOAD_TIMEOUT_SEC,
) -> Tuple[int, str, str]:
    """
    Lance un upload PlatformIO sur le port donné.
    BREEZLY_FLEET_FLASH=1 pour que le post-hook ne fasse pas le provision (fait par le script).
    """
    my_env = os.environ.copy()
    my_env["PIO_UPLOAD_ONLY"] = "1"
    my_env["BREEZLY_FLEET_FLASH"] = "1"
    if variant:
        my_env["DEVICE_VARIANT"] = variant
    cmd = [
        pio_exe,
        "run",
        "--environment", env_name,
        "-t", "upload",
        "--upload-port", port,
    ]
    return run_cmd(cmd, cwd=cwd, env=my_env, capture=True, timeout_sec=timeout_sec)


# ---------------------------------------------------------------------------
# Lecture MAC via esptool (dans le script, pas dans le hook)
# ---------------------------------------------------------------------------

def _find_esptool_from_platformio() -> Optional[List[str]]:
    """Retourne [python, esptool.py] si trouvé dans les packages PlatformIO."""
    for base in [
        os.environ.get("PLATFORMIO_CORE_DIR"),
        str(Path.home() / ".platformio"),
    ]:
        if not base:
            continue
        for name in ("esptool.py", "scripts/esptool.py"):
            path = Path(base) / "packages" / "tool-esptoolpy" / name
            if path.exists():
                return [sys.executable, str(path)]
    return None


def get_esptool_cmd() -> list:
    """Commande pour esptool : esptool des packages PlatformIO si présent, sinon python -m esptool."""
    fallback = _find_esptool_from_platformio()
    if fallback:
        return fallback
    return [sys.executable, "-m", "esptool"]


def check_esptool_available() -> None:
    """Vérifie que esptool est installé (pour lecture MAC). Sort en erreur sinon."""
    try:
        # --help retourne 0 et ne nécessite pas de port
        code, _, err = run_cmd(get_esptool_cmd() + ["--help"], capture=True, timeout_sec=10)
        if code != 0:
            raise RuntimeError(err or "esptool a échoué")
    except subprocess.TimeoutExpired:
        raise SystemExit("esptool --help a expiré (timeout).")
    except FileNotFoundError:
        raise SystemExit(
            "esptool introuvable. Installez-le avec: pip install esptool\n"
            "Requis pour la lecture MAC après flash en mode flotte."
        )
    except Exception as e:
        raise SystemExit(
            f"esptool non disponible: {e}\n"
            "Installez avec: pip install esptool"
        )


def read_mac_esptool(
    port: str,
    timeout_sec: int = MAC_READ_TIMEOUT_SEC,
    retries: int = MAC_READ_RETRIES,
) -> Tuple[Optional[str], Optional[str]]:
    """
    Lit le MAC via esptool read_mac. Retourne (mac_avec_colons, external_id) ou (None, None).
    external_id = PROV_<MAC 12 hex sans ':'>, aligné firmware/backend.
    """
    base_cmd = get_esptool_cmd()
    for attempt in range(1, retries + 1):
        try:
            cmd = base_cmd + ["read_mac", "--port", port]
            code, out, err = run_cmd(cmd, capture=True, timeout_sec=timeout_sec)
            if code != 0:
                out = out or err or ""
            text = (out + " " + err)
            # MAC: AA:BB:CC:DD:EE:FF ou format similaire
            m = re.search(r"MAC:\s*([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})", text)
            if not m:
                m = re.search(r"([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})", text)
            if m:
                mac_colons = m.group(1)
                mac_hex = mac_colons.replace(":", "").upper()
                if len(mac_hex) == 12:
                    external_id = f"PROV_{mac_hex}"
                    return mac_colons, external_id
        except subprocess.TimeoutExpired:
            pass
        except Exception:
            pass
        if attempt < retries:
            time.sleep(1)
    return None, None


# ---------------------------------------------------------------------------
# Provisioning backend Breezly (même logique que post_upload_register)
# ---------------------------------------------------------------------------

def provision_backend(
    api_url: str,
    factory_token: str,
    device_key_b64: str,
    external_id: str,
    variant: str,
    timeout_sec: int = PROVISION_TIMEOUT_SEC,
    retries: int = PROVISION_RETRIES,
) -> bool:
    """
    POST /api/internal/provision-device. Payload compatible avec l'existant.
    Retourne True si 2xx, False sinon.
    """
    url = api_url.rstrip("/") + "/api/internal/provision-device"
    payload = {
        "external_id": external_id,
        "deviceKeyB64": device_key_b64,
        "name": external_id,
        "type": "temperature",
        "location": "Bureau",
    }
    if variant:
        payload["variant"] = variant
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=data,
        headers={
            "Content-Type": "application/json",
            "X-Factory-Token": factory_token,
        },
        method="POST",
    )
    for _ in range(retries):
        try:
            with urllib.request.urlopen(req, timeout=timeout_sec) as resp:
                return 200 <= resp.status < 300
        except urllib.error.HTTPError as e:
            if 200 <= e.code < 300:
                return True
        except Exception:
            pass
        time.sleep(0.5)
    return False


# ---------------------------------------------------------------------------
# Pipeline par port : flash → pause → MAC → provision → résultat
# ---------------------------------------------------------------------------

def process_one_port(
    port: str,
    pio_exe: str,
    env_name: str,
    cwd: str,
    variant: Optional[str],
    fw_version: str,
    api_url: str,
    factory_token: str,
    device_key_b64: str,
) -> PortResult:
    """
    Pour un port : upload (avec retries), lecture MAC, provisioning, construction du résultat.
    Pas d'écriture EOL ici (faite en central après collecte).
    """
    res = PortResult(
        port=port,
        flash_ok=False,
        flash_attempts=0,
        fw_version=fw_version,
        variant=variant or "",
    )

    # 1. Upload avec retries
    for attempt in range(1, UPLOAD_RETRIES + 1):
        res.flash_attempts = attempt
        code, out, err = pio_upload_one(
            pio_exe, env_name, port, cwd, variant,
            timeout_sec=UPLOAD_TIMEOUT_SEC,
        )
        if code == 0:
            res.flash_ok = True
            break
        if attempt < UPLOAD_RETRIES:
            time.sleep(2)
    if not res.flash_ok:
        res.error_stage = "upload"
        res.remarks = "upload_failed"
        return res

    # 2. Pause courte (stabilisation port COM)
    time.sleep(POST_UPLOAD_DELAY_SEC)

    # 3. Lecture MAC
    mac_colons, external_id = read_mac_esptool(
        port,
        timeout_sec=MAC_READ_TIMEOUT_SEC,
        retries=MAC_READ_RETRIES,
    )
    res.mac = mac_colons
    res.external_id = external_id
    if not external_id:
        res.error_stage = "mac_read"
        res.remarks = "mac_read_failed"
        return res

    # 4. Provisioning backend (seulement si factory_token et device_key fournis)
    if factory_token and device_key_b64:
        res.provision_ok = provision_backend(
            api_url,
            factory_token,
            device_key_b64,
            external_id,
            res.variant,
            timeout_sec=PROVISION_TIMEOUT_SEC,
            retries=PROVISION_RETRIES,
        )
        if not res.provision_ok:
            res.error_stage = "provision"
            res.remarks = "provision_failed"
    else:
        res.remarks = "provision_skipped_no_secrets"

    return res


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Flash une flotte d'ESP32 (build unique + uploads parallèles). MAC et provision gérés dans le script.",
    )
    parser.add_argument("--env", required=True, help="Environnement PlatformIO (ex: esp32-wroom-32e-prod)")
    parser.add_argument("--pio-exe", default=DEFAULT_PIO_EXE, help="Chemin vers platformio.exe")
    parser.add_argument(
        "--jobs",
        type=int,
        default=DEFAULT_JOBS,
        help=f"Uploads en parallèle (défaut: {DEFAULT_JOBS})",
    )
    parser.add_argument(
        "--include",
        nargs="*",
        default=["cp210"],
        help="Filtre devices (port/desc/hwid)",
    )
    parser.add_argument(
        "--variant",
        choices=["STD", "PREMIUM"],
        help="Variant matériel (STD/PREMIUM) pour le backend",
    )
    parser.add_argument("--exclude", nargs="*", default=[], help="Exclure devices")
    parser.add_argument("--ports", nargs="*", default=[], help="Liste de ports (ex: COM3 COM4)")
    parser.add_argument("--no-build", action="store_true", help="Ne pas compiler")
    parser.add_argument("--eol-log", default=EOL_LOG_DEFAULT, help=f"Fichier EOL CSV (défaut: {EOL_LOG_DEFAULT})")
    parser.add_argument("--no-eol", action="store_true", help="Désactiver le journal EOL")
    parser.add_argument(
        "--operator",
        default=os.environ.get("EOL_OPERATOR", ""),
        help="Opérateur EOL (ou EOL_OPERATOR)",
    )
    parser.add_argument(
        "--api-url",
        default=os.environ.get("API_URL", DEFAULT_API_URL),
        help="URL backend Breezly pour le provisioning",
    )
    args = parser.parse_args()

    cwd = os.getcwd()
    ensure_platformio(args.pio_exe)

    factory_token, device_key_b64 = load_secrets(cwd)
    api_url = args.api_url or DEFAULT_API_URL
    if not factory_token and not device_key_b64:
        print(f"[{now()}] Attention: FACTORY_TOKEN / DEVICE_KEY_B64 absents → pas de provisioning backend.", file=sys.stderr)

    if not args.no_build:
        pio_build(args.pio_exe, args.env, cwd)

    if args.ports:
        ports = [normalize_port(p) for p in args.ports if normalize_port(p)]
    else:
        devices = list_ports_json(args.pio_exe, cwd)
        ports = pick_ports(devices, include=args.include, exclude=args.exclude)

    if not ports:
        raise SystemExit(
            "Aucun port trouvé.\n"
            "→ Vérifie les devices branchés, --include/--exclude, ou --ports COMx ..."
        )

    check_esptool_available()

    fw_version = get_fw_version(cwd)
    eol_log_path = Path(args.eol_log)
    if not eol_log_path.is_absolute():
        eol_log_path = Path(cwd) / eol_log_path
    if not args.no_eol:
        ensure_eol_log_header(eol_log_path)
        print(f"[{now()}] Journal EOL: {eol_log_path}")

    print(f"[{now()}] Ports ({len(ports)}): {', '.join(ports)}")
    print(f"[{now()}] Jobs={args.jobs}, FW={fw_version}")

    results: List[PortResult] = []
    failed_ports: list[str] = []

    def run_one(port: str) -> PortResult:
        return process_one_port(
            port,
            args.pio_exe,
            args.env,
            cwd,
            args.variant,
            fw_version,
            api_url,
            factory_token,
            device_key_b64,
        )

    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(run_one, p): p for p in ports}
        for fut in as_completed(futs):
            port = futs[fut]
            try:
                res = fut.result()
            except Exception as e:
                res = PortResult(
                    port=port,
                    flash_ok=False,
                    flash_attempts=0,
                    fw_version=fw_version,
                    variant=args.variant or "",
                    error_stage="exception",
                    remarks=str(e),
                )
            results.append(res)
            # Échec si flash KO ou si provision était attendue et a échoué
            if not res.flash_ok:
                failed_ports.append(port)
            elif res.external_id and factory_token and device_key_b64 and not res.provision_ok:
                failed_ports.append(port)

            status = "✅" if res.flash_ok and (not res.external_id or res.provision_ok) else "❌"
            print(f"[{now()}] {status} {res.port} flash={res.flash_ok} mac={bool(res.mac)} provision={res.provision_ok}")
            if res.error_stage:
                print(f"         → {res.error_stage}: {res.remarks}", file=sys.stderr)

            if not args.no_eol:
                dt = datetime.now()
                eol_ok, eol_remarks = eol_result_and_remarks(res)
                append_eol_row(
                    eol_log_path,
                    dt.strftime("%Y-%m-%d"),
                    dt.strftime("%H:%M:%S"),
                    res.mac or "",
                    res.external_id or "",
                    res.fw_version,
                    res.variant,
                    res.port,
                    eol_ok,
                    args.operator,
                    eol_remarks,
                )

    ok_count = len(ports) - len(failed_ports)
    print(f"\n[{now()}] Terminé. OK={ok_count} / FAIL={len(failed_ports)}")
    if not args.no_eol:
        print(f"[{now()}] EOL: {eol_log_path}")
    if failed_ports:
        print(f"Ports en échec: {', '.join(failed_ports)}", file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
