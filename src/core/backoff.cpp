/**
 * @file backoff.cpp
 * @brief Implémentation backoff exponentiel + jitter (overflow-safe millis).
 */
#include "backoff.h"
#include <cmath>
#include <cstdlib>

#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
#include <esp_random.h>
#define BACKOFF_RANDOM()  ((uint32_t)esp_random())
#else
#include <ctime>
#include <cstdlib>
static uint32_t backoff_random_val() {
  static bool seeded = false;
  if (!seeded) { srand((unsigned)time(nullptr)); seeded = true; }
  return (uint32_t)rand();
}
#define BACKOFF_RANDOM()  backoff_random_val()
#endif

Backoff::Backoff(const BackoffConfig& config)
  : _config(config), _attemptCount(0), _nextAttemptMs(0), _lastDelayMs(0) {}

void Backoff::reset() {
  _attemptCount = 0;
  _nextAttemptMs = 0;
  _lastDelayMs = 0;
}

uint32_t Backoff::computeDelay(uint32_t effectiveMinMs) const {
  uint32_t base = (effectiveMinMs > 0) ? effectiveMinMs : _config.minDelayMs;
  if (base > _config.maxDelayMs) base = _config.maxDelayMs;

  double powVal = 1.0;
  if (_attemptCount > 0) {
    powVal = pow((double)_config.factor, (double)_attemptCount);
  }
  uint64_t next = (uint64_t)((double)base * powVal);
  if (next > _config.maxDelayMs) next = _config.maxDelayMs;
  if (next < base) next = base;

  uint32_t jitterPct = _config.jitterPercent;
  if (jitterPct > 50) jitterPct = 50;
  uint32_t jitterRange = (uint32_t)((uint64_t)next * jitterPct / 100);
  uint32_t jitter = (jitterRange == 0) ? 0 : (BACKOFF_RANDOM() % (2 * jitterRange + 1)) - jitterRange;
  int64_t delay = (int64_t)next + (int64_t)jitter;
  if (delay < (int64_t)_config.minDelayMs) delay = _config.minDelayMs;
  if (delay > (int64_t)_config.maxDelayMs) delay = _config.maxDelayMs;
  return (uint32_t)delay;
}

void Backoff::onFailure(uint32_t nowMs, uint32_t effectiveMinMs) {
  _lastDelayMs = computeDelay(effectiveMinMs);
  _nextAttemptMs = nowMs + _lastDelayMs;
  _attemptCount++;
}

bool Backoff::shouldAttempt(uint32_t nowMs) const {
  if (_nextAttemptMs == 0) return true;
  // Overflow-safe: (nowMs - _nextAttemptMs) en uint32_t : si now >= next alors diff "petit"
  uint32_t diff = nowMs - _nextAttemptMs;
  return (diff < 0x80000000u);  // now a dépassé nextAttemptMs
}

BackoffState Backoff::getState() const {
  BackoffState s;
  s.attemptCount = _attemptCount;
  s.nextAttemptMs = _nextAttemptMs;
  s.lastDelayMs = _lastDelayMs;
  return s;
}

namespace wifi_backoff {
  uint32_t effectiveMinForReason(Reason r, uint32_t authFailMinMs) {
    if (r == AuthFail && authFailMinMs > 0) return authFailMinMs;
    return 0;
  }
}

namespace mqtt_backoff {
  uint32_t effectiveMinForReason(Reason r, uint32_t defaultMinMs) {
    (void)r;
    return defaultMinMs;  // même min pour tous pour l'instant
  }
}

#if defined(BACKOFF_SIM_TEST)
#include <Arduino.h>
void backoff_run_simulation(void) {
  BackoffConfig cfg = {
    1000,    // min 1s
    300000,  // max 5 min
    2.0f,
    10       // jitter ±10%
  };
  Backoff b(cfg);
  uint32_t now = 0;
  const int kAttempts = 12;
  for (int i = 0; i < kAttempts; i++) {
    b.onFailure(now, 0);
    uint32_t d = b.lastDelayMs();
    now += d;
    Serial.printf("[BACKOFF_SIM] attempt %d delay %lu ms (cap 5min)\n", i + 1, (unsigned long)d);
  }
  Serial.println("[BACKOFF_SIM] done");
}
#endif
