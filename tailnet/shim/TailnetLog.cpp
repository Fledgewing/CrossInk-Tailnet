// In-memory log of MicroLink and shim messages, for the Tailscale screen.
//
// The prebuilt framework compiles ESP_LOG output below ERROR out of its own
// libraries, and the readers' serial port is not always at hand. MicroLink and
// the shim are compiled with LOG_LOCAL_LEVEL=INFO (tailnet_build.py); this hook
// keeps their recent lines in RAM and still forwards everything to the console.
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "Tailnet.h"

namespace {

constexpr int LINES = 24;
constexpr int WIDTH = 120;
char s_lines[LINES][WIDTH];
int s_head = 0;   // next slot to write
int s_count = 0;  // lines held
uint32_t s_seq = 0;  // lines ever stored
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
vprintf_like_t s_prev = nullptr;

const char* const TAGS[] = {"tailnet",  "microlink", "ml_coord", "ml_coord_tls", "ml_derp",    "ml_filter", "ml_h2",
                            "ml_net_io", "ml_noise",  "ml_peer_nvs", "ml_stun",  "ml_tcp",     "ml_udp",    "ml_wg_mgr",
                            "ml_zc",    "wg"};

int hook(const char* fmt, va_list args) {
  va_list copy;
  va_copy(copy, args);
  char line[WIDTH];
  vsnprintf(line, sizeof(line), fmt, copy);
  va_end(copy);

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
  if (j) {
    portENTER_CRITICAL(&s_lock);
    memcpy(s_lines[s_head], clean, j + 1);
    s_head = (s_head + 1) % LINES;
    if (s_count < LINES) s_count++;
    s_seq++;
    portEXIT_CRITICAL(&s_lock);
  }
  return s_prev ? s_prev(fmt, args) : vprintf(fmt, args);
}

}  // namespace

extern "C" void tailnet_log_install(void) {
  if (s_prev) return;
  s_prev = esp_log_set_vprintf(hook);
  for (const char* tag : TAGS) esp_log_level_set(tag, ESP_LOG_INFO);
}

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
