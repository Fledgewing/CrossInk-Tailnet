// Fewer WiFi driver buffers on the ESP32-C3 (no PSRAM).
//
// Arduino's WiFiGeneric.cpp (compiled from source) fills wifi_init_config_t
// with WIFI_INIT_CONFIG_DEFAULT() and then, unless WiFi.useStaticBuffers(true)
// was called, sets static RX 4, dynamic RX 32, dynamic TX 32. The link wraps
// esp_wifi_init on the C3 build only (tailnet_build.py), and this wrapper lowers
// the memory-heavy counts in a copy of the caller's config before passing it on.
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
// Left alone: management buffers (5 static x ~500 B, needed while scanning) and
// the short management buffers (dynamic, < 64 B each).
#include "sdkconfig.h"

#if CONFIG_ML_LITE

#include <esp_wifi.h>

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

esp_err_t __real_esp_wifi_init(const wifi_init_config_t* config);

static int lower(int value, int cap) { return (value <= 0 || value > cap) ? cap : value; }

esp_err_t __wrap_esp_wifi_init(const wifi_init_config_t* config) {
  if (config == NULL || config->magic != WIFI_INIT_CONFIG_MAGIC) return __real_esp_wifi_init(config);
  wifi_init_config_t cfg = *config;
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
  return __real_esp_wifi_init(&cfg);
}

#endif  // CONFIG_ML_LITE
