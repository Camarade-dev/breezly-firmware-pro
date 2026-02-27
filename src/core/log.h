/**
 * Breezly — Logging centralisé par niveau (compile-time + option runtime).
 * En prod : DEBUG/TRACE désactivés (zéro coût). Pas de fuite de secrets (passwords, tokens, payload MQTT).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef BREEZLY_LOG_LEVEL
#define BREEZLY_LOG_LEVEL 3
#endif

#define BREEZLY_LOG_LEVEL_OFF   0
#define BREEZLY_LOG_LEVEL_ERROR 1
#define BREEZLY_LOG_LEVEL_WARN  2
#define BREEZLY_LOG_LEVEL_INFO  3
#define BREEZLY_LOG_LEVEL_DEBUG 4
#define BREEZLY_LOG_LEVEL_TRACE 5

#ifdef __cplusplus
extern "C" {
#endif

void breezly_log_impl(int level, const char* tag, const char* fmt, ...);

/* Niveau runtime optionnel (0 = utiliser compile-time). */
void breezly_log_set_level(int level);
int  breezly_log_get_level(void);

/* Helpers anti-leak : ne jamais logger en clair les secrets en prod. */
const char* breezly_log_redact(const char* s, int show_tail);
void breezly_log_hex_short(const uint8_t* buf, size_t len, size_t max_bytes);

#ifdef __cplusplus
}
#endif

/* Macros : zéro coût quand le niveau est désactivé (compile-time). */
#define _BREEZLY_LOG(level, tag, fmt, ...) \
  do { breezly_log_impl(level, tag, fmt, ##__VA_ARGS__); } while (0)

#if BREEZLY_LOG_LEVEL >= BREEZLY_LOG_LEVEL_ERROR
#define LOGE(tag, fmt, ...) _BREEZLY_LOG(1, tag, fmt, ##__VA_ARGS__)
#else
#define LOGE(tag, fmt, ...) do { } while (0)
#endif

#if BREEZLY_LOG_LEVEL >= BREEZLY_LOG_LEVEL_WARN
#define LOGW(tag, fmt, ...) _BREEZLY_LOG(2, tag, fmt, ##__VA_ARGS__)
#else
#define LOGW(tag, fmt, ...) do { } while (0)
#endif

#if BREEZLY_LOG_LEVEL >= BREEZLY_LOG_LEVEL_INFO
#define LOGI(tag, fmt, ...) _BREEZLY_LOG(3, tag, fmt, ##__VA_ARGS__)
#else
#define LOGI(tag, fmt, ...) do { } while (0)
#endif

#if BREEZLY_LOG_LEVEL >= BREEZLY_LOG_LEVEL_DEBUG
#define LOGD(tag, fmt, ...) _BREEZLY_LOG(4, tag, fmt, ##__VA_ARGS__)
#else
#define LOGD(tag, fmt, ...) do { } while (0)
#endif

#if BREEZLY_LOG_LEVEL >= BREEZLY_LOG_LEVEL_TRACE
#define LOGT(tag, fmt, ...) _BREEZLY_LOG(5, tag, fmt, ##__VA_ARGS__)
#else
#define LOGT(tag, fmt, ...) do { } while (0)
#endif

/* Helpers C++ pour redaction / hex (usage dans LOGD/LOGT uniquement). */
static inline const char* logRedact(const char* s, int showTail) {
  return breezly_log_redact(s, showTail);
}
static inline void logHexShort(const uint8_t* buf, size_t len, size_t maxBytes = 8) {
  breezly_log_hex_short(buf, len, maxBytes);
}
