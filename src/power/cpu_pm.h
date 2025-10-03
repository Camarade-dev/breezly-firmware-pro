// power/cpu_pm.h
#pragma once
extern "C" { 
#include "esp_pm.h" 
}

static inline void enableCpuPM(){
  esp_pm_config_t cfg = {};
  cfg.max_freq_mhz = 240;
  cfg.min_freq_mhz = 80;
  cfg.light_sleep_enable = true; // tickless idle -> light sleep auto
  esp_pm_configure(&cfg);
}
