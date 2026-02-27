/**
 * Breezly — Implémentation logging centralisé.
 * Serial.printf si disponible ; pas d'allocation dynamique en boucle.
 */
#include "log.h"
#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int s_runtime_level = 0;  /* 0 = use compile-time BREEZLY_LOG_LEVEL */

static const char* level_str(int level) {
  switch (level) {
    case 1: return "E";
    case 2: return "W";
    case 3: return "I";
    case 4: return "D";
    case 5: return "T";
    default: return "?";
  }
}

void breezly_log_set_level(int level) {
  s_runtime_level = level;
}

int breezly_log_get_level(void) {
  return s_runtime_level;
}

static int effective_level(void) {
  if (s_runtime_level > 0) return s_runtime_level;
  return BREEZLY_LOG_LEVEL;
}

void breezly_log_impl(int level, const char* tag, const char* fmt, ...) {
  if (level > effective_level()) return;
  const char* lstr = level_str(level);
  char buf[2];
  buf[0] = lstr[0];
  buf[1] = '\0';
  Serial.printf("[%s][%s] ", buf, tag ? tag : "");
  va_list ap;
  va_start(ap, fmt);
  char line[256];
  int n = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (n > 0) {
    Serial.println(line);
  } else {
    Serial.println();
  }
}

#define REDACT_BUF_SIZE 32
static char s_redact_buf[REDACT_BUF_SIZE];

const char* breezly_log_redact(const char* s, int show_tail) {
  if (!s) return "(null)";
  size_t len = strlen(s);
  if (len == 0) return "(empty)";
  if (show_tail <= 0) {
    snprintf(s_redact_buf, REDACT_BUF_SIZE, "***");
    return s_redact_buf;
  }
  if (show_tail >= REDACT_BUF_SIZE - 4) show_tail = REDACT_BUF_SIZE - 5;
  size_t tail = (size_t)show_tail;
  if (tail > len) tail = len;
  size_t redact_len = len - tail;
  size_t i = 0;
  for (; i < redact_len && i < (REDACT_BUF_SIZE - (size_t)show_tail - 2); i++)
    s_redact_buf[i] = '*';
  if (i < REDACT_BUF_SIZE - tail - 1) {
    for (size_t j = 0; j < tail && (i + j) < REDACT_BUF_SIZE - 1; j++)
      s_redact_buf[i + j] = s[len - tail + j];
    s_redact_buf[i + tail] = '\0';
  } else {
    s_redact_buf[REDACT_BUF_SIZE - 1] = '\0';
  }
  return s_redact_buf;
}

void breezly_log_hex_short(const uint8_t* buf, size_t len, size_t max_bytes) {
  if (!buf) return;
  if (max_bytes == 0) max_bytes = 8;
  size_t n = len > max_bytes ? max_bytes : len;
  for (size_t i = 0; i < n; i++) {
    Serial.printf("%02X", buf[i]);
  }
  if (len > max_bytes) Serial.print("...");
}
