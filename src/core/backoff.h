/**
 * @file backoff.h
 * @brief Backoff exponentiel générique avec jitter (Wi-Fi, MQTT, HTTP).
 * Non bloquant, overflow-safe pour millis(), réutilisable.
 */
#pragma once

#include <cstdint>

struct BackoffConfig {
  uint32_t minDelayMs;
  uint32_t maxDelayMs;
  float factor;
  uint8_t jitterPercent;  // ±N% (ex: 10 = ±10%)
};

struct BackoffState {
  uint32_t attemptCount;
  uint32_t nextAttemptMs;
  uint32_t lastDelayMs;
};

class Backoff {
public:
  explicit Backoff(const BackoffConfig& config);

  /** Réinitialise à l'état initial (après succès). */
  void reset();

  /**
   * Enregistre un échec et programme la prochaine tentative.
   * @param nowMs millis() actuel
   * @param effectiveMinMs 0 = utiliser config.minDelayMs ; sinon min forcé pour cet échec (ex: auth_fail 30s)
   */
  void onFailure(uint32_t nowMs, uint32_t effectiveMinMs = 0);

  /** Délai calculé pour la dernière tentative (pour log). */
  uint32_t lastDelayMs() const { return _lastDelayMs; }

  /** Faut-il tenter maintenant ? Overflow-safe. */
  bool shouldAttempt(uint32_t nowMs) const;

  /** État pour télémétrie. */
  BackoffState getState() const;

  /** Nombre d'échecs consécutifs. */
  uint32_t attemptCount() const { return _attemptCount; }

private:
  BackoffConfig _config;
  uint32_t _attemptCount;
  uint32_t _nextAttemptMs;
  uint32_t _lastDelayMs;

  uint32_t computeDelay(uint32_t effectiveMinMs) const;
};

// --- Policy helpers (raisons Wi‑Fi → effectiveMin pour onFailure) ---
namespace wifi_backoff {
  enum Reason : uint8_t {
    Other = 0,
    AuthFail,
    NoSsid,
    Timeout
  };
  /** effectiveMinMs à passer à onFailure (0 = use config default). */
  uint32_t effectiveMinForReason(Reason r, uint32_t authFailMinMs);
}

// --- Policy helpers MQTT ---
namespace mqtt_backoff {
  enum Reason : uint8_t {
    Other = 0,
    ConnectFail,
    BrokerUnreachable,
    TlsFail,
    DnsFail
  };
  uint32_t effectiveMinForReason(Reason r, uint32_t defaultMinMs);
}

#if defined(BACKOFF_SIM_TEST)
void backoff_run_simulation(void);
#endif
