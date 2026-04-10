#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Helpers partages pour l'identite device et le provisioning factory.

- normalisation des variants
- conversion MAC <-> external_id
- lecture de l'external_id sur la sortie serie du device
- parsing robuste de la sortie esptool read_mac
"""

from __future__ import annotations

import re
import time
from typing import Optional, Tuple

DEVICE_VARIANTS = ("STD", "PREMIUM", "B2B", "B2B_PMS")
EXTERNAL_ID_PREFIX = "PROV_"
EXTERNAL_ID_RE = re.compile(r"\bBREEZLY_EXTERNAL_ID=(PROV_[0-9A-Fa-f]{12})\b")
MAC_RE = re.compile(r"MAC:\s*([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})")
MAC_ANY_RE = re.compile(r"\b([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})\b")


def normalize_variant(value: Optional[str]) -> Optional[str]:
    """Normalise la variante en uppercase ; renvoie None si vide."""
    if value is None:
        return None
    normalized = str(value).strip().upper()
    return normalized or None


def external_id_from_mac(mac_value: str) -> Optional[str]:
    """Construit PROV_<12 hex> a partir d'un MAC avec ou sans ':'."""
    if not mac_value:
        return None
    mac_hex = str(mac_value).replace(":", "").strip().upper()
    if not re.fullmatch(r"[0-9A-F]{12}", mac_hex):
        return None
    return f"{EXTERNAL_ID_PREFIX}{mac_hex}"


def mac_colons_from_external_id(external_id: Optional[str]) -> Optional[str]:
    """Reconstitue AA:BB:CC:DD:EE:FF depuis PROV_AABBCCDDEEFF."""
    if not external_id:
        return None
    match = re.fullmatch(r"PROV_([0-9A-Fa-f]{12})", external_id.strip())
    if not match:
        return None
    mac_hex = match.group(1).upper()
    return ":".join(mac_hex[i:i + 2] for i in range(0, 12, 2))


def parse_mac_from_text(text: str) -> Optional[str]:
    """Extrait un MAC AA:BB:CC:DD:EE:FF depuis stdout/stderr."""
    if not text:
        return None
    match = MAC_RE.search(text) or MAC_ANY_RE.search(text)
    return match.group(1).upper() if match else None


def parse_external_id_from_text(text: str) -> Optional[str]:
    """Extrait BREEZLY_EXTERNAL_ID=PROV_xxx depuis les logs serie."""
    if not text:
        return None
    match = EXTERNAL_ID_RE.search(text)
    return match.group(1).upper() if match else None


def read_external_id_from_serial(
    port: str,
    timeout_sec: int = 12,
    baudrate: int = 115200,
) -> Tuple[Optional[str], str]:
    """
    Lit BREEZLY_EXTERNAL_ID=PROV_xxx sur le port serie.

    Retourne (external_id, diagnostic).
    - diagnostic = "ok" si trouve
    - sinon une courte raison exploitable dans les logs
    """
    try:
        import serial  # type: ignore
    except Exception:
        return None, "pyserial_not_installed"

    buffer = ""
    deadline = time.monotonic() + max(0.5, float(timeout_sec))

    try:
        ser = serial.Serial(port=port, baudrate=baudrate, timeout=0.25, write_timeout=0.25)
    except Exception as exc:
        return None, f"serial_open_failed:{exc.__class__.__name__}"

    try:
        try:
            ser.dtr = False
            ser.rts = False
        except Exception:
            pass
        try:
            ser.reset_input_buffer()
        except Exception:
            pass

        while time.monotonic() < deadline:
            chunk = ser.read(512)
            if not chunk:
                continue
            buffer += chunk.decode("utf-8", errors="ignore")
            external_id = parse_external_id_from_text(buffer)
            if external_id:
                return external_id, "ok"
    except Exception as exc:
        return None, f"serial_read_failed:{exc.__class__.__name__}"
    finally:
        try:
            ser.close()
        except Exception:
            pass

    return None, "serial_external_id_timeout"
