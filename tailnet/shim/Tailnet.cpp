// See Tailnet.h. One worker task owns the MicroLink handle; WiFi event
// callbacks only post requests to it, because microlink_stop() can block for
// up to CONFIG_ML_STOP_TIMEOUT_MS and must not run on Arduino's event task.
#include "Tailnet.h"

#include <WiFi.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <lwip/dns.h>
#include <lwip/tcpip.h>
#include <nvs.h>
#include <stdio.h>
#include <string.h>

#include <atomic>

extern "C" {
#include "microlink.h"
void tailnet_ml_set_priority_peer(microlink_t* ml, uint32_t vpn_ip);     // ml_priority.c
void tailnet_ml_relay_codes(const microlink_t* ml, uint32_t vpn_ip, char* home, char* peer, size_t len);
void tailnet_ml_set_forced_region(microlink_t* ml, uint16_t region);
uint16_t tailnet_ml_home_region(const microlink_t* ml);
uint16_t tailnet_ml_peer_region(const microlink_t* ml, uint32_t vpn_ip);
int tailnet_ml_regions(const microlink_t* ml, uint16_t* ids, char (*codes)[8], int max);
}

// Arduino's esp32-hal-log.h (pulled in by WiFi.h) turns ESP_LOGx into its own
// log_x macros, which CORE_DEBUG_LEVEL compiles out. Write to the ESP-IDF log
// directly so TailnetLog.cpp captures these lines.
#define TLOG(level, letter, fmt, ...) \
  esp_log_write(level, TAG, letter " (%lu) %s: " fmt "\n", (unsigned long)esp_log_timestamp(), TAG, ##__VA_ARGS__)
#define TLOGE(fmt, ...) TLOG(ESP_LOG_ERROR, "E", fmt, ##__VA_ARGS__)
#define TLOGW(fmt, ...) TLOG(ESP_LOG_WARN, "W", fmt, ##__VA_ARGS__)
#define TLOGI(fmt, ...) TLOG(ESP_LOG_INFO, "I", fmt, ##__VA_ARGS__)

namespace {

const char* TAG = "tailnet";
const char* NVS_NS = "tailnet";
const char* NVS_KEY_ENABLED = "enabled";
const char* NVS_KEY_JOINED = "joined";  // set once connected; cleared by logout
const char* NVS_KEY_PIN = "pin_ip";     // learned priority peer (host byte order)
const char* NVS_KEY_RELAY_MODE = "relay_mode";  // tailnet_relay_mode_t
const char* NVS_KEY_RELAY_FIXED = "relay_rgn";  // region picked by the user
const char* NVS_KEY_SERVER_RGN = "srv_rgn";     // learned home region of the server
const char* NVS_KEY_SERVER_IP = "srv_ip";       // tailnet address last connected to

enum Request : uint8_t { REQ_WIFI_UP, REQ_WIFI_DOWN, REQ_RECHECK, REQ_LOGOUT, REQ_REPIN, REQ_RELAY };

QueueHandle_t s_queue = nullptr;
microlink_t* s_ml = nullptr;  // owned by the worker task
bool s_running = false;       // worker task only
bool s_wifi_up = false;       // worker task only

std::atomic<int> s_status{TAILNET_OFF};
std::atomic<uint32_t> s_generation{0};
std::atomic<uint32_t> s_ip{0};
std::atomic<int> s_enabled{-1};  // -1 = not loaded from NVS yet
std::atomic<int> s_joined{-1};   // -1 = not loaded from NVS yet
std::atomic<uint32_t> s_pin{0};  // priority peer; loaded once at the first start
bool s_pin_loaded = false;       // worker task only
// Peers already re-pinned this WiFi session (no restart loops for a peer that
// is offline or not on the tailnet). Written by callers of tailnet_ensure_peer.
portMUX_TYPE s_tried_lock = portMUX_INITIALIZER_UNLOCKED;
uint32_t s_tried[4];

// The login URL is written by MicroLink's task (via the state callback) and
// read by UI tasks.
portMUX_TYPE s_url_lock = portMUX_INITIALIZER_UNLOCKED;
char s_url[MICROLINK_AUTH_URL_MAX];
static_assert(TAILNET_LOGIN_URL_MAX >= MICROLINK_AUTH_URL_MAX, "Tailnet.h buffer too small");

// Relay settings, cached; written by the UI task (setter) and the worker.
std::atomic<int> s_relay_mode{-1};      // -1 = not loaded
std::atomic<int> s_relay_fixed{0};
std::atomic<int> s_server_rgn{0};
std::atomic<uint32_t> s_server_ip{0};
bool s_relay_matched = false;  // worker only: one match-server restart per WiFi session

uint8_t loadFlag(const char* key, std::atomic<int>& cache);
void storeFlag(const char* key, std::atomic<int>& cache, bool value);

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
        TLOGW("Tailscale login needed: %s", url);
        s_generation.fetch_add(1);
      }
    }
  }
  s_ip.store(st == ML_STATE_CONNECTED ? microlink_get_vpn_ip(s_ml) : 0);
  if (st == ML_STATE_CONNECTED) storeFlag(NVS_KEY_JOINED, s_joined, true);
  setStatus(mapState(st));
}

void onState(microlink_t*, microlink_state_t, void*) { post(REQ_RECHECK); }

uint8_t loadFlag(const char* key, std::atomic<int>& cache) {
  int cached = cache.load();
  if (cached >= 0) return (uint8_t)cached;
  uint8_t v = 0;
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
    nvs_get_u8(h, key, &v);
    nvs_close(h);
  }
  cache.store(v ? 1 : 0);
  return v ? 1 : 0;
}

void storeFlag(const char* key, std::atomic<int>& cache, bool value) {
  if (loadFlag(key, cache) == (value ? 1 : 0)) return;
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u8(h, key, value ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
  }
  cache.store(value ? 1 : 0);
  s_generation.fetch_add(1);
}

// Off until turned on in Settings > Tailscale.
bool loadEnabled() { return loadFlag(NVS_KEY_ENABLED, s_enabled) != 0; }

void stopMl() {
  if (!s_running) return;
  // Retry: stop returns ESP_ERR_TIMEOUT (and frees nothing) if a task is
  // stuck in a blocking call; a later call resumes the wait.
  for (int i = 0; i < 4; i++) {
    if (microlink_stop(s_ml) == ESP_OK) break;
    TLOGW("microlink_stop timed out; retrying");
  }
  s_running = false;
  s_ip.store(0);
  portENTER_CRITICAL(&s_url_lock);
  s_url[0] = '\0';
  portEXIT_CRITICAL(&s_url_lock);
  setStatus(TAILNET_OFF);
}

uint32_t nvsGetU32(const char* key) {
  uint32_t v = 0;
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
    nvs_get_u32(h, key, &v);
    nvs_close(h);
  }
  return v;
}

void nvsSetU32(const char* key, uint32_t v) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u32(h, key, v);
    nvs_commit(h);
    nvs_close(h);
  }
}

void loadRelay() {
  if (s_relay_mode.load() >= 0) return;
  s_relay_fixed.store((int)nvsGetU32(NVS_KEY_RELAY_FIXED));
  s_server_rgn.store((int)nvsGetU32(NVS_KEY_SERVER_RGN));
  s_server_ip.store(nvsGetU32(NVS_KEY_SERVER_IP));
  uint32_t m = nvsGetU32(NVS_KEY_RELAY_MODE);
  s_relay_mode.store(m <= TAILNET_RELAY_FIXED ? (int)m : 0);
}

// The home region to force at start (0 = let MicroLink pick the closest).
uint16_t forcedRegion() {
  loadRelay();
  switch (s_relay_mode.load()) {
    case TAILNET_RELAY_FIXED:
      return (uint16_t)s_relay_fixed.load();
    case TAILNET_RELAY_MATCH_SERVER:
      return (uint16_t)s_server_rgn.load();  // 0 until learned: closest for now
    default:
      return 0;
  }
}

uint32_t serverIp() {
  loadRelay();
  uint32_t ip = s_server_ip.load();
  return ip ? ip : s_pin.load();
}

// Match-server mode: once the server is in the peer table, follow its home
// region if ours differs (remembered, so later starts use it right away).
void matchServerRegion() {
  if (!s_ml || s_relay_matched || s_relay_mode.load() != TAILNET_RELAY_MATCH_SERVER) return;
  const uint16_t theirs = tailnet_ml_peer_region(s_ml, serverIp());
  const uint16_t ours = tailnet_ml_home_region(s_ml);
  if (!theirs || !ours || theirs == ours) return;
  s_relay_matched = true;
  TLOGW("Relay region %u -> %u to match the server; restarting MicroLink", ours, theirs);
  nvsSetU32(NVS_KEY_SERVER_RGN, theirs);
  s_server_rgn.store(theirs);
  stopMl();  // reconcile() starts it again with the new region
}

uint32_t loadPin() {
  if (!s_pin_loaded) {
    uint32_t v = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
      nvs_get_u32(h, NVS_KEY_PIN, &v);
      nvs_close(h);
    }
    s_pin.store(v);
    s_pin_loaded = true;
  }
  return s_pin.load();
}

void storePin(uint32_t ip) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u32(h, NVS_KEY_PIN, ip);
    nvs_commit(h);
    nvs_close(h);
  }
  s_pin.store(ip);
  s_pin_loaded = true;
}

void startMl() {
  if (s_running) return;
  if (!s_ml) {
    microlink_config_t cfg = {};
    cfg.auth_key = nullptr;     // interactive login; nothing secret in the image
    cfg.device_name = nullptr;  // esp32-<mac>; rename it in the admin console
    s_ml = microlink_init(&cfg);
    if (!s_ml) {
      TLOGE("microlink_init failed (free heap %u)", (unsigned)esp_get_free_heap_size());
      setStatus(TAILNET_FAILED);
      return;
    }
    microlink_set_state_callback(s_ml, onState, nullptr);
  }
  // The control plane only resends a dropped peer on a fresh map, so the pin
  // is (re)applied on every start.
  tailnet_ml_set_priority_peer(s_ml, loadPin());
  tailnet_ml_set_forced_region(s_ml, forcedRegion());
  setStatus(TAILNET_STARTING);
  const esp_err_t err = microlink_start(s_ml);
  if (err != ESP_OK) {
    TLOGE("microlink_start: %s", esp_err_to_name(err));
    setStatus(TAILNET_FAILED);
    return;
  }
  s_running = true;
  TLOGI("MicroLink started");
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
          s_relay_matched = false;
          portENTER_CRITICAL(&s_tried_lock);
          memset(s_tried, 0, sizeof(s_tried));
          portEXIT_CRITICAL(&s_tried_lock);
          break;
        case REQ_LOGOUT:
          stopMl();
          if (s_ml) {
            microlink_destroy(s_ml);
            s_ml = nullptr;
          }
          microlink_factory_reset();
          storeFlag(NVS_KEY_JOINED, s_joined, false);
          TLOGW("Tailnet identity erased");
          break;
        case REQ_REPIN:
          // A peer the reader needs is not in MicroLink's table (a large
          // tailnet fills it). Restart with it pinned: the fresh map lets it
          // evict the least recently used peer. Keys and login are kept.
          if (s_running) {
            TLOGW("Pinning %u.%u.%u.%u as priority peer; restarting MicroLink", (unsigned)(s_pin >> 24) & 0xff,
                  (unsigned)(s_pin >> 16) & 0xff, (unsigned)(s_pin >> 8) & 0xff, (unsigned)s_pin & 0xff);
            stopMl();
          }
          break;
        case REQ_RELAY:
          if (s_running) {
            TLOGW("Relay setting changed; restarting MicroLink");
            stopMl();
          }
          s_relay_matched = false;
          break;
        case REQ_RECHECK:
          break;
      }
      reconcile();
    }
    refreshFromHandle();
    if (s_running && tailnet_status() == TAILNET_CONNECTED) {
      matchServerRegion();
      if (!s_running) reconcile();
    }
  }
}

void onWifiEvent(arduino_event_id_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      // lwIP's DNS cache outlives a change of network: a name answered by
      // the home network's split DNS (a LAN address) would be reused on
      // another network until its TTL ran out. Start each network clean.
      tcpip_callback([](void*) { dns_clear_cache(); }, nullptr);
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
  tailnet_log_install();
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
  storeFlag(NVS_KEY_ENABLED, s_enabled, enabled);
  post(REQ_RECHECK);
}

extern "C" bool tailnet_joined(void) { return loadFlag(NVS_KEY_JOINED, s_joined) != 0; }

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

namespace {
bool peerKnown(uint32_t ip) {
  if (!s_ml) return false;
  const int n = microlink_get_peer_count(s_ml);
  microlink_peer_info_t info;
  for (int i = 0; i < n; i++) {
    if (microlink_get_peer_info(s_ml, i, &info) == ESP_OK && info.vpn_ip == ip) return true;
  }
  return false;
}

// True the first time ip is seen this WiFi session.
bool markTried(uint32_t ip) {
  bool first = true;
  portENTER_CRITICAL(&s_tried_lock);
  int free_slot = -1;
  for (int i = 0; i < 4; i++) {
    if (s_tried[i] == ip) first = false;
    if (!s_tried[i] && free_slot < 0) free_slot = i;
  }
  if (first) s_tried[free_slot >= 0 ? free_slot : 0] = ip;
  portEXIT_CRITICAL(&s_tried_lock);
  return first;
}
}  // namespace

#define IP_ARGS(ip) (unsigned)((ip) >> 24) & 0xff, (unsigned)((ip) >> 16) & 0xff, (unsigned)((ip) >> 8) & 0xff, \
                    (unsigned)(ip) & 0xff

static std::atomic<uint32_t> s_last_target{0};

// The tailnet address the reader connects to is its BookOrbit server (the only
// tailnet host CrossInk talks to); match-server mode follows its relay region.
static void rememberServer(uint32_t ip) {
  loadRelay();
  if (s_server_ip.exchange(ip) != ip) nvsSetU32(NVS_KEY_SERVER_IP, ip);
}

extern "C" bool tailnet_ensure_peer(uint32_t ip, uint32_t timeout_ms) {
  s_last_target.store(ip);
  if (!tailnet_wait_connected(timeout_ms)) {
    TLOGW("connect %u.%u.%u.%u: tailnet not connected", IP_ARGS(ip));
    return false;
  }
  // A fresh map may still be filling the table just after CONNECTED.
  for (int i = 0; i < 30 && !peerKnown(ip); i++) vTaskDelay(pdMS_TO_TICKS(100));
  if (peerKnown(ip)) {
    rememberServer(ip);
    return true;
  }
  if (ip == s_pin.load() || !markTried(ip)) {
    TLOGW("connect %u.%u.%u.%u: not a peer of this reader (check ACLs)", IP_ARGS(ip));
    return false;
  }
  TLOGW("connect %u.%u.%u.%u: not in peer table (%d peers); pinning", IP_ARGS(ip),
           s_ml ? microlink_get_peer_count(s_ml) : 0);
  storePin(ip);  // remembered across reboots: pinned from the next start on
  post(REQ_REPIN);
  // Wait for the restart to begin, then for it to finish.
  for (int i = 0; i < 50 && tailnet_status() == TAILNET_CONNECTED; i++) vTaskDelay(pdMS_TO_TICKS(100));
  if (!tailnet_wait_connected(timeout_ms)) return false;
  for (int i = 0; i < 50 && !peerKnown(ip); i++) vTaskDelay(pdMS_TO_TICKS(100));
  const bool known = peerKnown(ip);
  if (known) {
    TLOGW("connect %u.%u.%u.%u: peer added after pinning", IP_ARGS(ip));
    rememberServer(ip);
  } else {
    TLOGW("connect %u.%u.%u.%u: still not a peer after pinning (check ACLs)", IP_ARGS(ip));
  }
  return known;
}

extern "C" void tailnet_get_diag(tailnet_diag_t* out) {
  if (!out) return;
  *out = {};
  out->peer_capacity = CONFIG_ML_MAX_PEERS;
  out->pin_ip = s_pin.load();
  out->target_ip = s_last_target.load();
  if (!out->target_ip) out->target_ip = out->pin_ip;
  if (!s_ml || tailnet_status() != TAILNET_CONNECTED) return;
  out->peer_count = microlink_get_peer_count(s_ml);
  microlink_peer_info_t info;
  for (int i = 0; i < out->peer_count; i++) {
    if (microlink_get_peer_info(s_ml, i, &info) == ESP_OK && info.vpn_ip == out->target_ip && out->target_ip) {
      out->target_known = true;
      out->target_online = info.online;
      out->target_direct = info.direct_path;
    }
  }
  tailnet_ml_relay_codes(s_ml, out->target_ip, out->home_relay, out->target_relay, sizeof(out->home_relay));
}

extern "C" tailnet_relay_mode_t tailnet_relay_mode(void) {
  loadRelay();
  return (tailnet_relay_mode_t)s_relay_mode.load();
}

extern "C" uint16_t tailnet_relay_fixed_region(void) {
  loadRelay();
  return (uint16_t)s_relay_fixed.load();
}

extern "C" void tailnet_set_relay(tailnet_relay_mode_t mode, uint16_t region) {
  loadRelay();
  if (mode == TAILNET_RELAY_FIXED) {
    if (!region) return;
    nvsSetU32(NVS_KEY_RELAY_FIXED, region);
    s_relay_fixed.store(region);
  }
  nvsSetU32(NVS_KEY_RELAY_MODE, mode);
  s_relay_mode.store(mode);
  s_generation.fetch_add(1);
  post(REQ_RELAY);
}

extern "C" int tailnet_relay_regions(uint16_t* ids, char (*codes)[8], int max) {
  return s_ml ? tailnet_ml_regions(s_ml, ids, codes, max) : 0;
}

extern "C" void tailnet_relay_code(uint16_t id, char* out, size_t len) {
  if (!out || !len) return;
  out[0] = 0;
  if (!id) return;
  uint16_t ids[32];
  char codes[32][8];
  const int n = tailnet_relay_regions(ids, codes, 32);
  for (int i = 0; i < n; i++) {
    if (ids[i] == id) {
      snprintf(out, len, "%s", codes[i]);
      return;
    }
  }
  snprintf(out, len, "region %u", (unsigned)id);
}
