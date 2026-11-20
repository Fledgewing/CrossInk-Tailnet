// In-memory log of MicroLink and shim messages, for the Tailscale screen.
//
// The prebuilt framework has CONFIG_LOG_MAXIMUM_LEVEL=ERROR, and
// esp_log_level_set() cannot raise a tag above it: INFO and WARN lines from
// MicroLink and the shim are dropped inside esp_log before any vprintf hook
// sees them. So the link wraps esp_log (MicroLink's ESP_LOGx) and
// esp_log_write (the shim's TLOG) (tailnet_build.py): lines from our tags are
// kept here first, then passed on unchanged (the console still filters them).
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "Tailnet.h"

namespace {

constexpr int LINES = 40;
constexpr int WIDTH = 120;
char s_lines[LINES][WIDTH];
int s_head = 0;   // next slot to write
int s_count = 0;  // lines held
uint32_t s_seq = 0;  // lines ever stored
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

const char* const TAGS[] = {"tailnet",  "microlink", "ml_coord", "ml_coord_tls", "ml_derp",    "ml_filter", "ml_h2",
                            "ml_net_io", "ml_noise",  "ml_peer_nvs", "ml_stun",  "ml_tcp",     "ml_udp",    "ml_wg_mgr",
                            "ml_zc",    "wg"};

bool ours(const char* tag) {
  if (!tag) return false;
  for (const char* t : TAGS) {
    if (strcmp(tag, t) == 0) return true;
  }
  return false;
}

void store(const char* fmt, va_list args) {
  char line[WIDTH];
  vsnprintf(line, sizeof(line), fmt, args);

  // Drop ANSI colour codes and the trailing newline; skip blank lines.
  char clean[WIDTH];
  size_t j = 0;
  for (size_t i = 0; line[i] && j < sizeof(clean) - 1; i++) {
    if (line[i] == '\033') {
      while (line[i] && line[i] != 'm') i++;
      if (!line[i]) break;
      continue;
    }
    if (line[i] == '\n' || line[i] == '\r') continue;
    clean[j++] = line[i];
  }
  clean[j] = '\0';
  if (!j) return;
  portENTER_CRITICAL(&s_lock);
  memcpy(s_lines[s_head], clean, j + 1);
  s_head = (s_head + 1) % LINES;
  if (s_count < LINES) s_count++;
  s_seq++;
  portEXIT_CRITICAL(&s_lock);
}

}  // namespace

extern "C" {
// ESP_LOGx (log v1) expands to esp_log(); the format already carries the
// level letter, timestamp and tag.
void __wrap_esp_log(esp_log_config_t config, const char* tag, const char* format, ...) {
  va_list args;
  va_start(args, format);
  if (!config.opts.constrained_env && ours(tag)) {
    va_list copy;
    va_copy(copy, args);
    store(format, copy);
    va_end(copy);
  }
  esp_log_va(config, tag, format, args);
  va_end(args);
}

void __wrap_esp_log_write(esp_log_level_t level, const char* tag, const char* format, ...) {
  va_list args;
  va_start(args, format);
  if (ours(tag)) {
    va_list copy;
    va_copy(copy, args);
    store(format, copy);
    va_end(copy);
  }
  esp_log_writev(level, tag, format, args);
  va_end(args);
}
}

extern "C" void tailnet_log_install(void) {}

extern "C" bool tailnet_log_line(int newest_index, char* buf, size_t len) {
  if (!buf || !len) return false;
  bool ok = false;
  portENTER_CRITICAL(&s_lock);
  if (newest_index >= 0 && newest_index < s_count) {
    const int slot = (s_head - 1 - newest_index + LINES) % LINES;
    strncpy(buf, s_lines[slot], len - 1);
    buf[len - 1] = '\0';
    ok = true;
  }
  portEXIT_CRITICAL(&s_lock);
  return ok;
}

extern "C" uint32_t tailnet_log_seq(void) {
  portENTER_CRITICAL(&s_lock);
  const uint32_t v = s_seq;
  portEXIT_CRITICAL(&s_lock);
  return v;
}
