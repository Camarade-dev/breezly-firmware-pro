#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Flash en parallèle une flotte d'ESP32 (hub USB multi-ports) et remplit automatiquement
le journal EOL (traçabilité commerciale). Une seule commande : brancher les PCBs, lancer le script.
"""

import argparse
import csv
import json
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime
from pathlib import Path

DEFAULT_PIO_EXE = r"C:\Users\stris\.platformio\penv\Scripts\platformio.exe"
EOL_LOG_DEFAULT = "docs/EOL_LOG.csv"
APP_CONFIG_PATH = "src/app_config.h"

def now():
    return datetime.now().strftime("%H:%M:%S")

def get_fw_version(cwd):
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

def parse_external_id_from_upload_output(out):
    """
    Extrait MAC (12 hex) et external_id (PROV_XXXXXXXXXXXX) de la sortie du post_upload_register.
    Retourne (mac_with_colons, external_id) ou (None, None).
    """
    if not out:
        return None, None
    m = re.search(r"external_id pour provision:\s*PROV_([0-9A-Fa-f]{12})", out)
    if not m:
        return None, None
    hex12 = m.group(1).upper()
    mac_colons = ":".join(hex12[i : i + 2] for i in range(0, 12, 2))
    external_id = f"PROV_{hex12}"
    return mac_colons, external_id

def ensure_eol_log_header(eol_log_path):
    """Crée le fichier EOL avec en-tête CSV si absent (compatible specs traçabilité)."""
    path = Path(eol_log_path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        return
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["Date", "Heure", "MAC", "external_id", "Version_FW", "EOL_resultat", "Operateur", "Remarques"])

def append_eol_row(eol_log_path, date, time, mac, external_id, version_fw, result, operator, remarks):
    """Ajoute une ligne au journal EOL (append, thread-safe depuis le thread principal)."""
    path = Path(eol_log_path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "a", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow([date, time, mac or "", external_id or "", version_fw, result, operator or "", remarks or ""])

def run_cmd(cmd, cwd=None, env=None, capture=True):
    """
    Run command and return (returncode, stdout, stderr).
    """
    if capture:
        p = subprocess.run(
            cmd,
            cwd=cwd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            shell=False,
        )
        return p.returncode, p.stdout, p.stderr
    else:
        p = subprocess.run(cmd, cwd=cwd, env=env, shell=False)
        return p.returncode, "", ""

def ensure_platformio(pio_exe):
    if not Path(pio_exe).exists():
        raise FileNotFoundError(
            f"platformio.exe introuvable: {pio_exe}\n"
            "→ Mets le bon chemin via --pio-exe"
        )

def list_ports_json(pio_exe, cwd):
    cmd = [pio_exe, "device", "list", "--json-output"]
    code, out, err = run_cmd(cmd, cwd=cwd, capture=True)
    if code != 0:
        raise RuntimeError(f"[device list] échec ({code})\n{err.strip()}")
    try:
        return json.loads(out)
    except json.JSONDecodeError as e:
        raise RuntimeError(f"[device list] JSON invalide: {e}\nSortie:\n{out}")

def normalize_port(p):
    # On retourne le port tel que PlatformIO le veut (COMx sous Windows).
    return (p or "").strip()

def pick_ports(devices, include=None, exclude=None):
    """
    devices: list of dict from platformio device list JSON
    include/exclude: list of substrings filters applied to 'description' or 'hwid' or 'port'
    """
    include = include or []
    exclude = exclude or []

    ports = []
    for d in devices:
        port = normalize_port(d.get("port"))
        desc = (d.get("description") or "")
        hwid = (d.get("hwid") or "")

        hay = f"{port} {desc} {hwid}".lower()

        if include:
            ok = any(s.lower() in hay for s in include)
            if not ok:
                continue

        if exclude:
            bad = any(s.lower() in hay for s in exclude)
            if bad:
                continue

        if port:
            ports.append(port)

    # uniq stable
    seen = set()
    uniq = []
    for p in ports:
        if p not in seen:
            seen.add(p)
            uniq.append(p)
    return uniq

def pio_build(pio_exe, env_name, cwd):
    cmd = [pio_exe, "run", "--environment", env_name]
    print(f"[{now()}] Build: {env_name}")
    code, out, err = run_cmd(cmd, cwd=cwd, capture=True)
    if out.strip():
        print(out.rstrip())
    if code != 0:
        if err.strip():
            print(err.rstrip(), file=sys.stderr)
        raise RuntimeError(f"Build échoué ({code})")
    print(f"[{now()}] Build OK")

def pio_upload_one(pio_exe, env_name, port, cwd, variant=None):
    # 1. On prépare une copie de l'environnement actuel
    my_env = os.environ.copy()
    # 2. On active notre "Cheat Code" pour geler le timestamp
    my_env["PIO_UPLOAD_ONLY"] = "1"
    if variant:
        # Transmet le variant au hook post_upload_register.py
        my_env["DEVICE_VARIANT"] = variant
    
    cmd = [
        pio_exe, 
        "run", 
        "--environment", env_name, 
        "-t", "upload", 
        # ON RETIRE "-t", "nobuild" ICI car PIO va détecter "Up-to-date" tout seul
        "--upload-port", port
    ]
    
    # On passe my_env à la commande
    code, out, err = run_cmd(cmd, cwd=cwd, env=my_env, capture=True)
    return code, out, err

def main():
    parser = argparse.ArgumentParser(
        description="Flash une flotte d'ESP32 via PlatformIO (build unique + uploads parallèles)."
    )
    parser.add_argument(
        "--env",
        required=True,
        help="Nom de l'environnement PlatformIO (ex: esp32-wroom-32e-prod)",
    )
    parser.add_argument(
        "--pio-exe",
        default=DEFAULT_PIO_EXE,
        help="Chemin complet vers platformio.exe",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=3,
        help="Nombre d'uploads en parallèle (recommandé: 2-4)",
    )
    parser.add_argument(
        "--include",
        nargs="*",
        default=["cp210"],
        help="Filtre: conserve seulement les devices dont (port/desc/hwid) contient un de ces mots (ex: CP210 CH340)",
    )
    parser.add_argument(
        "--variant",
        choices=["STD", "PREMIUM"],
        help="Variant matériel des devices flashés dans cette flotte (STD ou PREMIUM). Transmis au backend pour la calibration température.",
    )

    parser.add_argument(
        "--exclude",
        nargs="*",
        default=[],
        help="Filtre: exclut les devices dont (port/desc/hwid) contient un de ces mots",
    )
    parser.add_argument(
        "--ports",
        nargs="*",
        default=[],
        help="Si fourni, ignore l’auto-détection et utilise cette liste (ex: COM3 COM4 COM5)",
    )
    parser.add_argument(
        "--no-build",
        action="store_true",
        help="Ne compile pas (upload direct). Utile si tu as déjà build.",
    )
    parser.add_argument(
        "--eol-log",
        default=EOL_LOG_DEFAULT,
        help=f"Fichier journal EOL (CSV) à remplir automatiquement. Défaut: {EOL_LOG_DEFAULT}",
    )
    parser.add_argument(
        "--no-eol",
        action="store_true",
        help="Désactive l'écriture du journal EOL (flash uniquement).",
    )
    parser.add_argument(
        "--operator",
        default=os.environ.get("EOL_OPERATOR", ""),
        help="Nom de l'opérateur pour la traçabilité EOL (ou variable EOL_OPERATOR).",
    )
    args = parser.parse_args()

    cwd = os.getcwd()
    ensure_platformio(args.pio_exe)

    # Build une seule fois
    if not args.no_build:
        pio_build(args.pio_exe, args.env, cwd)

    # Ports
    if args.ports:
        ports = [normalize_port(p) for p in args.ports if normalize_port(p)]
    else:
        devices = list_ports_json(args.pio_exe, cwd)
        ports = pick_ports(devices, include=args.include, exclude=args.exclude)

    if not ports:
        raise SystemExit(
            "Aucun port trouvé.\n"
            "→ Vérifie que tes Breezly sont branchés.\n"
            "→ Option: --ports COM3 COM4 ...\n"
            "→ Option: --include CP210 CH340\n"
        )

    fw_version = get_fw_version(cwd)
    eol_log_path = Path(args.eol_log) if not Path(args.eol_log).is_absolute() else Path(args.eol_log)
    if not eol_log_path.is_absolute():
        eol_log_path = Path(cwd) / eol_log_path
    if not args.no_eol:
        ensure_eol_log_header(str(eol_log_path))
        print(f"[{now()}] Journal EOL: {eol_log_path}")

    print(f"[{now()}] Ports détectés ({len(ports)}): {', '.join(ports)}")
    print(f"[{now()}] Upload parallèle: jobs={args.jobs} (FW {fw_version})")

    results = {}
    failed = []

    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(pio_upload_one, args.pio_exe, args.env, p, cwd, args.variant): p for p in ports}

        for fut in as_completed(futs):
            port = futs[fut]
            try:
                code, out, err = fut.result()
            except Exception as e:
                code, out, err = 1, "", f"Exception: {e}"

            results[port] = (code, out, err)
            dt = datetime.now()
            date_str = dt.strftime("%Y-%m-%d")
            time_str = dt.strftime("%H:%M:%S")

            if code == 0:
                print(f"[{now()}] ✅ {port} OK")
                if not args.no_eol:
                    mac_colons, external_id = parse_external_id_from_upload_output(out)
                    if external_id:
                        append_eol_row(
                            str(eol_log_path),
                            date_str,
                            time_str,
                            mac_colons or "",
                            external_id,
                            fw_version,
                            "OK",
                            args.operator,
                            "",
                        )
                    else:
                        append_eol_row(
                            str(eol_log_path),
                            date_str,
                            time_str,
                            "",
                            "",
                            fw_version,
                            "KO",
                            args.operator,
                            "register_failed_or_no_external_id",
                        )
            else:
                print(f"[{now()}] ❌ {port} FAIL ({code})", file=sys.stderr)
                failed.append(port)
                if not args.no_eol:
                    append_eol_row(
                        str(eol_log_path),
                        date_str,
                        time_str,
                        "",
                        "",
                        fw_version,
                        "KO",
                        args.operator,
                        "upload_failed",
                    )

            # Optionnel: afficher un extrait utile si fail
            if code != 0:
                if out.strip():
                    print(out.rstrip(), file=sys.stderr)
                if err.strip():
                    print(err.rstrip(), file=sys.stderr)

    print(f"\n[{now()}] Terminé. OK={len(ports)-len(failed)} / FAIL={len(failed)}")
    if not args.no_eol:
        print(f"[{now()}] Journal EOL mis à jour: {eol_log_path}")
    if failed:
        print(f"Ports en échec: {', '.join(failed)}", file=sys.stderr)
        sys.exit(2)

if __name__ == "__main__":
    main()
