Import("env")
import time
import os

# Nom du fichier temporaire pour stocker la signature du dernier build
SIG_FILE = ".last_build_sig"

# On regarde si la variable d'environnement "PIO_UPLOAD_ONLY" est définie
if "PIO_UPLOAD_ONLY" in os.environ:
    # MODE UPLOAD PARALLÈLE :
    # On essaie de relire la signature existante pour ne PAS déclencher de rebuild
    if os.path.exists(SIG_FILE):
        with open(SIG_FILE, "r") as f:
            sig = f.read().strip()
    else:
        # Fallback au cas où
        sig = str(int(time.time()))
else:
    # MODE BUILD NORMAL :
    # On génère une nouvelle signature (l'heure actuelle) et on la sauvegarde
    sig = str(int(time.time()))
    with open(SIG_FILE, "w") as f:
        f.write(sig)

# On injecte la signature
env.Append(CPPDEFINES=[("FLASH_BUILD_SIG", f'\\"{sig}\\"')])
print(f"FLASH_BUILD_SIG = {sig} (Mode: {'UPLOAD_ONLY' if 'PIO_UPLOAD_ONLY' in os.environ else 'NEW_BUILD'})")