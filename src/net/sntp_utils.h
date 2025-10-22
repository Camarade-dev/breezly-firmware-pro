#pragma once
#include <stdint.h>

// Horloge "dure" (seuil 2024-01-01)
bool timeIsSaneHard();

// Compat : certains fichiers appellent encore timeIsSane()
// On wrappe vers la version "dure" pour éviter les liens manquants.
inline bool timeIsSane() { return timeIsSaneHard(); }

// SNTP start/stop
void startSNTPAfterConnected();
void stopSNTP();

// Gate horloge TLS-ready (attend SNTP puis fallback HTTP Date)
void ensureTlsClockReady(uint32_t maxWaitMs = 20000);
