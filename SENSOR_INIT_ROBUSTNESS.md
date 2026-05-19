# Sensor init robustness — changes & manual validation

Scope: make sensor init resilient (warm reboot, AHT decoupling, single PMS
UART owner, PMS init order, diagnostics). No product behavior change beyond
robustness. No architecture rewrite.

## What changed

### 1. BMP581 warm-reboot safe init — `src/sensors/bmp581.cpp`
- Root cause: `bmp5_init()` requires the clear-on-read POR bit
  (`power_up_check`, bmp5.c:1487). On a warm reboot the BMP581 stays powered
  and that bit was consumed by the previous session → permanent `BMP5_E_POWER_UP`.
- `bmp581Init()` now bounded-retries: on failure it issues a **raw** soft-reset
  (write `BMP5_SOFT_RESET_CMD` to `BMP5_REG_CMD`, then settle) and retries.
  We deliberately do **not** call Bosch `bmp5_soft_reset()` — that wrapper
  re-reads INT_STATUS to verify (bmp5.c:447) and consumes the same POR bit
  `bmp5_init()` then needs.
- Config re-applied after successful init. `s_initialized` accurate.
- `bmp581EnsureInitialized()` added. `bmp581Read()` self-heals: if not
  initialized it retries init from the read path, throttled
  (`BMP581_REINIT_RETRY_MS`, 30 s). A failed init is never permanent.

### 2. SCD41 warm-reboot safe init — `src/sensors/scd41.cpp`
- Same bug class, found in field logs (`err=268 = WriteError|I2cAddressNack`):
  `start_periodic_measurement` is NACKed when the SCD4x is already in periodic
  mode from a prior session.
- `scd41Init()` now issues `stopPeriodicMeasurement()` first (idle even if
  already running; the lib enforces the 500 ms settle), then
  `startPeriodicMeasurement()`, bounded-retry. `s_initialized` accurate.
- `scd41EnsureInitialized()` added. `scd41Read()` self-heals (throttled
  `SCD41_REINIT_RETRY_MS`, 30 s), independent of AHT health.

### 3. BMP/SCD decoupled from AHT21 — `src/main.cpp`
- Separate health paths. AHT, BMP581 and SCD41 are read and judged
  independently. An AHT failure no longer suppresses BMP/SCD reads or the
  whole publish; AHT/ENS payload fields are gated on `haveAht`, BMP/SCD
  fields on their own flags. LED air-score still requires AHT (uses T/HR/AQI).

### 4. PMS5003 single Serial2 owner — `src/sensors/sensors.cpp`
- `pmsTask` is the **only** code that reads Serial2; it maintains the cached
  `gPms` under `gPmsMutex`.
- Removed the cross-core `while (PMS.available()) PMS.read();` drain from
  `pmsSampleBurstBlocking()` (two readers on two cores desynced frames).
- `seq0` is now captured **after** warmup so only post-warmup frames count.

### 5. PMS init order — `src/main.cpp` / `sensors.cpp`
- Order is now: `pmsInitPins(PMS_SET_PIN)` → `pmsSleep()` →
  `pmsTaskStart(PMS_RX_PIN, PMS_TX_PIN)`. `PMS.begin()` moved into
  `pmsTaskStart()` **before** the task is created, so the task can never run
  `PMS.begin()` before the SET strapping pin is configured. Pins are
  `#define`d in `src/app_config.h`.

### 6. Diagnostics — `sensors.cpp`, `bmp581.*`, `scd41.*`
- `Bmp581Diag` / `Scd41Diag` / `PmsDiag` getters.
- `sensorsLogDiagnostics()` emits one compact `[I][DIAG]` line: reset reason,
  per-sensor state, BMP soft-reset / SCD stop-recovery usage, PMS
  ok/checksum/bad/timeout counters. Called from the publish path, throttled
  to ~10 min — never in a tight loop.

## Manual validation

No firmware unit-test harness exists (`test/` is the PlatformIO placeholder),
so validate on hardware. Build/flash:

```
pio run -e esp32-wroom-32e-prod -t upload
pio device monitor -b 115200
```

1. **Cold power cycle** (unplug ≥10 s, replug):
   - `[I][BMP581] initialized` and `[I][SCD41] initialized, periodic
     measurement started`. `DIAG`: `BMP=1 ... SCD41=1`.
2. **Warm reboot** (`ESP.restart()` / press EN, no power unplug):
   - BMP may show `initialized (soft-reset recovery)`, SCD41 may show
     `(stop+retry recovery)` — both must end at `=1`. **No physical unplug
     required.**
3. **OTA-like reboot** (trigger OTA or factory reset → `ESP.restart()`):
   - Same as (2): both sensors come up; `DIAG` confirms.
4. **AHT decoupling** (unplug AHT21 only, or force NaN):
   - `safeSensorRead` fails, but `pressure_pa` / `co2_ndir_ppm` still appear
     in the MQTT payload; BMP/SCD `DIAG` stay `=1`.
5. **PMS over ≥5 sample cycles** (~3 min apart in day mode):
   - No checksum/timeout storm. `DIAG` `chkErr`/`timeout` stay ~0 across
     repeated wake/sample/sleep.
6. **MQTT payload over ≥10 min**:
   - `prod/capteurs/qualite_air` carries `pressure_pa`, `temperature_bmp`,
     `co2_ndir_ppm` consistently. Distinguish real NDIR `co2_ndir_ppm`
     (~400–1200 ppm indoors, reacts to breath in seconds) from ENS160
     `eCO2` (smoother, derived).

Pass criteria: BMP581 and SCD41 both reach `=1` after cold, warm and
OTA reboots without a physical power cycle; PMS frames stay synchronized;
an AHT fault never suppresses BMP/SCD.
