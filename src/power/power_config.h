// ===== Duty-cycle capteurs =====
#define ENS_READ_PERIOD_MS_DAY      (30UL * 1000UL)
#define ENS_READ_PERIOD_MS_NIGHT    (120UL * 1000UL)

#define PMS_SAMPLE_PERIOD_MS_DAY    (3UL * 60UL * 1000UL)   // wake toutes 3 min
#define PMS_SAMPLE_PERIOD_MS_NIGHT  (10UL * 60UL * 1000UL)  // wake toutes 10 min
#define PMS_WARMUP_MS               (30UL * 1000UL)         // temps de mesure

// Fenêtre “interactive” après publish (réception commandes)
#define INTERACTIVE_WINDOW_MS       (30UL * 1000UL)

// Option : activer un vrai deep sleep par lot (sinon = light/modem sleep)
#define USE_DEEP_SLEEP              0
