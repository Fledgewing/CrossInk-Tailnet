// See Tailnet.h. One worker task owns the MicroLink handle; WiFi event
// callbacks only post requests to it, because microlink_stop() can block for
// up to CONFIG_ML_STOP_TIMEOUT_MS and must not run on Arduino's event task.
#include "Tailnet.h"

#include <WiFi.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <nvs.h>
#include <string.h>

#include <atomic>

extern "C" {
#include "microlink.h"
}

namespace {

const char* TAG = "tailnet";
const char* NVS_NS = "tailnet";
const char* NVS_KEY_ENABLED = "enabled";

enum Request : uint8_t { REQ_WIFI_UP, REQ_WIFI_DOWN, REQ_RECHECK, REQ_LOGOUT };

QueueHandle_t s_queue = nullptr;
microlink_t* s_ml = nullptr;  // owned by the worker task
bool s_running = false;       // worker task only
bool s_wifi_up = false;       // worker task only

std::atomic<int> s_status{TAILNET_OFF};
std::atomic<uint32_t> s_generation{0};
std::atomic<uint32_t> s_ip{0};
std::atomic<int> s_enabled{-1};  // -1 = not loaded from NVS yet

// The login URL is written by MicroLink's task (via the state callback) and
// read by UI tasks.
portMUX_TYPE s_url_lock = portMUX_INITIALIZER_UNLOCKED;
char s_url[MICROLINK_AUTH_URL_MAX];
static_assert(TAILNET_LOGIN_URL_MAX >= MICROLINK_AUTH_URL_MAX, "Tailnet.h buffer too small");

void setStatus(tailnet_status_t st) {
  if (s_status.exchange(st) != st) s_generation.fetch_add(1);
}

void post(Request r) {
  if (s_queue) xQueueSend(s_queue, &r, 0);
}

tailnet_status_t mapState(microlink_state_t st) {
  switch (st) {
    case ML_STATE_CONNECTED:
      return TAILNET_CONNECTED;
    case ML_STATE_RECONNECTING:
      return TAILNET_RECONNECTING;
    case ML_STATE_NEEDS_LOGIN:
      return TAILNET_NEEDS_LOGIN;
    case ML_STATE_AUTH_FAILED:
    case ML_STATE_ERROR:
      return TAILNET_FAILED;
    case ML_STATE_IDLE:
      return TAILNET_OFF;
    default:
      return TAILNET_STARTING;
  }
}

// MicroLink only reports CONNECTED, RECONNECTING and AUTH_FAILED through the
// callback, so the worker also polls while running (see workerTask).
void refreshFromHandle() {
  if (!s_ml || !s_running) return;
  const microlink_state_t st = microlink_get_state(s_ml);
  if (st == ML_STATE_NEEDS_LOGIN) {
    char url[MICROLINK_AUTH_URL_MAX] = {};
    if (microlink_get_auth_url(s_ml, url, sizeof(url)) == ESP_OK && url[0]) {
      bool changed;
      portENTER_CRITICAL(&s_url_lock);
      changed = strcmp(url, s_url) != 0;
      memcpy(s_url, url, sizeof(s_url));
      portEXIT_CRITICAL(&s_url_lock);
      if (changed) {
        ESP_LOGW(TAG, "Tailscale login needed: %s", url);
        s_generation.fetch_add(1);
      }
    }
  }
  s_ip.store(st == ML_STATE_CONNECTED ? microlink_get_vpn_ip(s_ml) : 0);
  setStatus(mapState(st));
}

void onState(microlink_t*, microlink_state_t, void*) { post(REQ_RECHECK); }

bool loadEnabled() {
  int cached = s_enabled.load();
  if (cached >= 0) return cached != 0;
  uint8_t v = 1;  // on by default: this firmware exists to be on the tailnet
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
    nvs_get_u8(h, NVS_KEY_ENABLED, &v);
    nvs_close(h);
  }
  s_enabled.store(v ? 1 : 0);
  return v != 0;
}

void stopMl() {
  if (!s_running) return;
  // Retry: stop returns ESP_ERR_TIMEOUT (and frees nothing) if a task is
  // stuck in a blocking call; a later call resumes the wait.
  for (int i = 0; i < 4; i++) {
    if (microlink_stop(s_ml) == ESP_OK) break;
    ESP_LOGW(TAG, "microlink_stop timed out; retrying");
  }
  s_running = false;
  s_ip.store(0);
  portENTER_CRITICAL(&s_url_lock);
  s_url[0] = '\0';
  portEXIT_CRITICAL(&s_url_lock);
  setStatus(TAILNET_OFF);
}

void startMl() {
  if (s_running) return;
  if (!s_ml) {
    microlink_config_t cfg = {};
    cfg.auth_key = nullptr;     // interactive login; nothing secret in the image
    cfg.device_name = nullptr;  // esp32-<mac>; rename it in the admin console
    s_ml = microlink_init(&cfg);
    if (!s_ml) {
      ESP_LOGE(TAG, "microlink_init failed (free heap %u)", (unsigned)esp_get_free_heap_size());
      setStatus(TAILNET_FAILED);
      return;
    }
    microlink_set_state_callback(s_ml, onState, nullptr);
  }
  setStatus(TAILNET_STARTING);
  const esp_err_t err = microlink_start(s_ml);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "microlink_start: %s", esp_err_to_name(err));
    setStatus(TAILNET_FAILED);
    return;
  }
  s_running = true;
  ESP_LOGI(TAG, "MicroLink started");
}

void reconcile() {
  const bool want = s_wifi_up && loadEnabled();
  if (want && !s_running) startMl();
  if (!want && s_running) stopMl();
}

void workerTask(void*) {
  for (;;) {
    Request r;
    // While running, wake every second to pick up states MicroLink does not
    // report through its callback (NEEDS_LOGIN, the URL, the VPN address).
    const TickType_t wait = s_running ? pdMS_TO_TICKS(1000) : portMAX_DELAY;
    if (xQueueReceive(s_queue, &r, wait) == pdTRUE) {
      switch (r) {
        case REQ_WIFI_UP:
          s_wifi_up = true;
          break;
        case REQ_WIFI_DOWN:
          s_wifi_up = false;
          break;
        case REQ_LOGOUT:
          stopMl();
          if (s_ml) {
            microlink_destroy(s_ml);
            s_ml = nullptr;
          }
          microlink_factory_reset();
          ESP_LOGW(TAG, "Tailnet identity erased");
          break;
        case REQ_RECHECK:
          break;
      }
      reconcile();
    }
    refreshFromHandle();
  }
}

void onWifiEvent(arduino_event_id_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      post(REQ_WIFI_UP);
      break;
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    case ARDUINO_EVENT_WIFI_STA_STOP:
      post(REQ_WIFI_DOWN);
      break;
    default:
      break;
  }
}

}  // namespace

// Arduino's initArduino() calls initVariant() from app_main, before setup():
// the scheduler is running and every static object (Arduino's Network event
// list included) is constructed. The core's definition is weak and empty, and
// neither CrossInk, the FreeInk SDK nor the S3/C3 variants define one, so this
// strong definition is the hook. A future clash would fail the link loudly.
extern "C" void initVariant() {
  s_queue = xQueueCreate(8, sizeof(Request));
  // MicroLink's own tasks do the network work; this one only sequences
  // start/stop, but microlink_init() and NVS writes need some stack.
  xTaskCreate(workerTask, "tailnet", 6144, nullptr, 3, nullptr);
  WiFi.onEvent(onWifiEvent, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(onWifiEvent, ARDUINO_EVENT_WIFI_STA_LOST_IP);
  WiFi.onEvent(onWifiEvent, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent(onWifiEvent, ARDUINO_EVENT_WIFI_STA_STOP);
}

extern "C" tailnet_status_t tailnet_status(void) { return (tailnet_status_t)s_status.load(); }

extern "C" bool tailnet_login_url(char* buf, size_t len) {
  if (!buf || len == 0) return false;
  portENTER_CRITICAL(&s_url_lock);
  strncpy(buf, s_url, len - 1);
  buf[len - 1] = '\0';
  portEXIT_CRITICAL(&s_url_lock);
  return buf[0] != '\0' && tailnet_status() == TAILNET_NEEDS_LOGIN;
}

extern "C" uint32_t tailnet_ip(void) { return s_ip.load(); }

extern "C" uint32_t tailnet_generation(void) { return s_generation.load(); }

extern "C" bool tailnet_enabled(void) { return loadEnabled(); }

extern "C" void tailnet_set_enabled(bool enabled) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u8(h, NVS_KEY_ENABLED, enabled ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
  }
  s_enabled.store(enabled ? 1 : 0);
  s_generation.fetch_add(1);
  post(REQ_RECHECK);
}

extern "C" void tailnet_logout(void) { post(REQ_LOGOUT); }

extern "C" uint32_t tailnet_resolve(const char* name) {
  // Only meaningful while connected; the handle outlives stop/start, and
  // microlink_resolve reads the peer table it keeps.
  if (!s_ml || tailnet_status() != TAILNET_CONNECTED || !name) return 0;
  char q[128];
  size_t n = strnlen(name, sizeof(q) - 1);
  memcpy(q, name, n);
  q[n] = '\0';
  if (n && q[n - 1] == '.') q[n - 1] = '\0';  // "host.tailnet.ts.net."
  return microlink_resolve(s_ml, q);
}

extern "C" bool tailnet_wait_connected(uint32_t timeout_ms) {
  const TickType_t start = xTaskGetTickCount();
  for (;;) {
    const tailnet_status_t st = tailnet_status();
    if (st == TAILNET_CONNECTED) return true;
    // Only a start in progress is worth waiting for; off, failed or waiting
    // for a login will not change within a request's timeout.
    if (st != TAILNET_STARTING && st != TAILNET_RECONNECTING) return false;
    if ((xTaskGetTickCount() - start) >= pdMS_TO_TICKS(timeout_ms)) return false;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
