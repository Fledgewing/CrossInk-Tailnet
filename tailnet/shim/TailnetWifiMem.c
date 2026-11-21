// WiFi init: fewer driver buffers on the ESP32-C3 (no PSRAM), and the other
// esp_wifi_init cuts (TailnetSysSwitches.h; per-env values in tailnet_build.py).
//
// Arduino's WiFiGeneric.cpp (compiled from source) fills wifi_init_config_t
// with WIFI_INIT_CONFIG_DEFAULT() and then, unless WiFi.useStaticBuffers(true)
// was called, sets static RX 4, dynamic RX 32, dynamic TX 32. The link wraps
// esp_wifi_init (tailnet_build.py), and this wrapper changes a copy of the
// caller's config before passing it on.
// The struct comes from the same framework headers the caller and the prebuilt
// library use, so its layout and magic are untouched; only values change.
//
// What each change buys (ESP-IDF esp_wifi Kconfig help and the "Wi-Fi Buffer
// Usage" section of the ESP32-C3 Wi-Fi guide; buffers are ~1.6 KB each):
//  - static RX 4 -> 3: static RX buffers are allocated in esp_wifi_init and held
//    until esp_wifi_deinit. One fewer is ~1.6 KB for the whole WiFi session.
//    3 is the guide's "Minimum" rank for the C3; Kconfig allows 2..25.
//  - dynamic RX 32 -> 8: allocated per received frame and held until lwIP is
//    done with it (TCP keeps unread segments). Bounds the RX worst case at
//    ~13 KB instead of ~51 KB. TCP_WND is 5760 (4 x MSS), so one HTTPS or relay
//    connection never has more than 4 full segments in flight. Must stay >=
//    static RX (Kconfig).
//  - dynamic TX 32 -> 8: same, for frames lwIP hands the driver; bounds TX at
//    ~13 KB instead of ~51 KB. TCP_SND_BUF is 5744 (4 x MSS). A full queue makes
//    lwIP retry, it does not drop the connection.
//  - AMPDU TX off: the reader sends little (requests, ACKs). Without it the
//    driver keeps no TX block-ack session nor up to TX_BA_WIN (6) frames waiting
//    for a block ack (~10 KB worst case).
//  - AMPDU RX stays on (APs expect it, and downloads need it), with the RX BA
//    window kept <= min(2 x static RX, dynamic RX) as the guide requires: 6.
// The buffer cuts above are TAILNET_WIFI_SMALL_BUFFERS (CONFIG_ML_LITE, the C3).
//
// Further cuts, each its own switch:
//  - TAILNET_WIFI_RX_MGMT: static RX management buffers 5 -> N (Kconfig range
//    1..10; ~500 B each). They take beacons, probe responses and auth/assoc
//    frames; fewer of them can drop scan results in a crowded band, so it is a
//    trade-off (the C3 only).
//  - TAILNET_WIFI_NO_NVS: nvs_enable 0. The driver then neither loads a saved
//    WiFi config from NVS at init nor writes one on esp_wifi_set_config.
//    CrossInk owns the credentials (WifiCredentialStore) and both of its join
//    paths call WiFi.persistent(false) (SavedWifiConnect.cpp,
//    WifiSelectionActivity.cpp), which makes Arduino set WIFI_STORAGE_RAM;
//    nothing calls WiFi.begin() without credentials. So the SDK's NVS copy is
//    never read, and the only change is that paths which bring WiFi up before
//    persistent(false) (the web server's AP mode, OTA, font download) stop
//    writing one. PHY calibration lives in its own NVS namespace (esp_phy),
//    untouched.
//  - TAILNET_WIFI_ALLOC_COUNT (measurement, off): counts the driver's own heap
//    use through a copy of its OS adapter table (osi_funcs), whose allocation
//    entries add up the bytes each block really takes
//    (heap_caps_get_allocated_size). TailnetStacks.c prints the totals at
//    each stack report.
// Left alone: feature_caps (WPA3 SAE and the rest) and the short management
// buffers (dynamic, < 64 B each).
#include "sdkconfig.h"

#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <esp_private/wifi_os_adapter.h>

#include "TailnetSysSwitches.h"

#ifndef TAILNET_WIFI_SMALL_BUFFERS
#define TAILNET_WIFI_SMALL_BUFFERS CONFIG_ML_LITE
#endif

void tailnet_log_emit(const char* format, ...);  // TailnetLog.cpp

// Overridable for measurement builds (tailnet_build.py). ESP-IDF's floors:
// static RX >= 2, dynamic TX >= 1; the RX BA window is capped below.
#ifndef TAILNET_WIFI_STATIC_RX
#define TAILNET_WIFI_STATIC_RX 3
#endif
#ifndef TAILNET_WIFI_DYNAMIC_RX
#define TAILNET_WIFI_DYNAMIC_RX 8
#endif
#ifndef TAILNET_WIFI_DYNAMIC_TX
#define TAILNET_WIFI_DYNAMIC_TX 8
#endif

#if TAILNET_WIFI_ALLOC_COUNT
// The driver keeps the osi_funcs pointer for as long as WiFi is initialised.
static wifi_osi_funcs_t s_osi;
static void* (*s_malloc)(size_t);
static void (*s_free)(void*);
static void* (*s_malloc_internal)(size_t);
static void* (*s_realloc_internal)(void*, size_t);
static void* (*s_calloc_internal)(size_t, size_t);
static void* (*s_zalloc_internal)(size_t);
static void* (*s_wifi_malloc)(size_t);
static void* (*s_wifi_realloc)(void*, size_t);
static void* (*s_wifi_calloc)(size_t, size_t);
static void* (*s_wifi_zalloc)(size_t);
static size_t s_live, s_peak, s_total;  // updated with atomics: any task may allocate

static void* counted(void* p) {
  if (p) {
    const size_t n = heap_caps_get_allocated_size(p);
    const size_t live = __atomic_add_fetch(&s_live, n, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_total, n, __ATOMIC_RELAXED);
    size_t peak = __atomic_load_n(&s_peak, __ATOMIC_RELAXED);
    while (live > peak && !__atomic_compare_exchange_n(&s_peak, &peak, live, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
  }
  return p;
}
static void uncount(void* p) {
  if (p) __atomic_sub_fetch(&s_live, heap_caps_get_allocated_size(p), __ATOMIC_RELAXED);
}
static void* c_malloc(size_t n) { return counted(s_malloc(n)); }
static void c_free(void* p) {
  uncount(p);
  s_free(p);
}
static void* c_malloc_internal(size_t n) { return counted(s_malloc_internal(n)); }
static void* c_calloc_internal(size_t n, size_t m) { return counted(s_calloc_internal(n, m)); }
static void* c_zalloc_internal(size_t n) { return counted(s_zalloc_internal(n)); }
static void* c_wifi_malloc(size_t n) { return counted(s_wifi_malloc(n)); }
static void* c_wifi_calloc(size_t n, size_t m) { return counted(s_wifi_calloc(n, m)); }
static void* c_wifi_zalloc(size_t n) { return counted(s_wifi_zalloc(n)); }
// realloc frees the old block only when it returns a new one.
static void* c_realloc(void* (*real)(void*, size_t), void* p, size_t n) {
  const size_t before = p ? heap_caps_get_allocated_size(p) : 0;
  void* q = real(p, n);
  if (!q && n) return q;  // failed: the old block is untouched
  if (p) __atomic_sub_fetch(&s_live, before, __ATOMIC_RELAXED);
  return n ? counted(q) : q;
}
static void* c_realloc_internal(void* p, size_t n) { return c_realloc(s_realloc_internal, p, n); }
static void* c_wifi_realloc(void* p, size_t n) { return c_realloc(s_wifi_realloc, p, n); }

static wifi_osi_funcs_t* countingOsi(wifi_osi_funcs_t* real) {
  if (!real) return real;
  s_osi = *real;
  s_malloc = real->_malloc;
  s_free = real->_free;
  s_malloc_internal = real->_malloc_internal;
  s_realloc_internal = real->_realloc_internal;
  s_calloc_internal = real->_calloc_internal;
  s_zalloc_internal = real->_zalloc_internal;
  s_wifi_malloc = real->_wifi_malloc;
  s_wifi_realloc = real->_wifi_realloc;
  s_wifi_calloc = real->_wifi_calloc;
  s_wifi_zalloc = real->_wifi_zalloc;
  s_osi._malloc = c_malloc;
  s_osi._free = c_free;
  s_osi._malloc_internal = c_malloc_internal;
  s_osi._realloc_internal = c_realloc_internal;
  s_osi._calloc_internal = c_calloc_internal;
  s_osi._zalloc_internal = c_zalloc_internal;
  s_osi._wifi_malloc = c_wifi_malloc;
  s_osi._wifi_realloc = c_wifi_realloc;
  s_osi._wifi_calloc = c_wifi_calloc;
  s_osi._wifi_zalloc = c_wifi_zalloc;
  return &s_osi;
}

bool tailnet_wifi_alloc_stats(size_t* live, size_t* peak, size_t* total) {
  *live = __atomic_load_n(&s_live, __ATOMIC_RELAXED);
  *peak = __atomic_load_n(&s_peak, __ATOMIC_RELAXED);
  *total = __atomic_load_n(&s_total, __ATOMIC_RELAXED);
  return true;
}
#else
bool tailnet_wifi_alloc_stats(size_t* live, size_t* peak, size_t* total) {
  (void)live;
  (void)peak;
  (void)total;
  return false;
}
#endif  // TAILNET_WIFI_ALLOC_COUNT

#if TAILNET_WIFI_SMALL_BUFFERS
static int lower(int value, int cap) { return (value <= 0 || value > cap) ? cap : value; }
#endif

esp_err_t __real_esp_wifi_init(const wifi_init_config_t* config);

esp_err_t __wrap_esp_wifi_init(const wifi_init_config_t* config) {
  if (config == NULL || config->magic != WIFI_INIT_CONFIG_MAGIC) return __real_esp_wifi_init(config);
  wifi_init_config_t cfg = *config;
#if TAILNET_WIFI_SMALL_BUFFERS
  cfg.static_rx_buf_num = lower(cfg.static_rx_buf_num, TAILNET_WIFI_STATIC_RX);
  cfg.dynamic_rx_buf_num = lower(cfg.dynamic_rx_buf_num, TAILNET_WIFI_DYNAMIC_RX);
  if (cfg.dynamic_rx_buf_num < cfg.static_rx_buf_num) cfg.dynamic_rx_buf_num = cfg.static_rx_buf_num;
  if (cfg.tx_buf_type == 1) cfg.dynamic_tx_buf_num = lower(cfg.dynamic_tx_buf_num, TAILNET_WIFI_DYNAMIC_TX);
  cfg.ampdu_tx_enable = 0;
  if (cfg.ampdu_rx_enable) {
    int win = 2 * cfg.static_rx_buf_num;
    if (cfg.dynamic_rx_buf_num < win) win = cfg.dynamic_rx_buf_num;
    if (cfg.rx_ba_win > win) cfg.rx_ba_win = win;
  }
  tailnet_log_emit("I tailnet: WiFi buffers static RX %d->%d, dynamic RX %d->%d, dynamic TX %d->%d, "
                   "AMPDU TX %d->0, RX BA window %d->%d\n",
                   config->static_rx_buf_num, cfg.static_rx_buf_num, config->dynamic_rx_buf_num, cfg.dynamic_rx_buf_num,
                   config->dynamic_tx_buf_num, cfg.dynamic_tx_buf_num, config->ampdu_tx_enable, config->rx_ba_win,
                   cfg.rx_ba_win);
#endif
#if TAILNET_WIFI_RX_MGMT >= 1 && TAILNET_WIFI_RX_MGMT <= 10
  if (cfg.rx_mgmt_buf_num > TAILNET_WIFI_RX_MGMT) cfg.rx_mgmt_buf_num = TAILNET_WIFI_RX_MGMT;
#endif
#if TAILNET_WIFI_NO_NVS
  cfg.nvs_enable = 0;
#endif
#if TAILNET_WIFI_ALLOC_COUNT
  cfg.osi_funcs = countingOsi(cfg.osi_funcs);
#endif
#if TAILNET_WIFI_RX_MGMT || TAILNET_WIFI_NO_NVS || TAILNET_STACK_LOG
  const size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const esp_err_t err = __real_esp_wifi_init(&cfg);
  tailnet_log_emit("I tailnet: esp_wifi_init: RX mgmt %d->%d, NVS %d->%d; heap %u -> %u\n", config->rx_mgmt_buf_num,
                   cfg.rx_mgmt_buf_num, config->nvs_enable, cfg.nvs_enable, (unsigned)before,
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  return err;
#else
  return __real_esp_wifi_init(&cfg);
#endif
}
