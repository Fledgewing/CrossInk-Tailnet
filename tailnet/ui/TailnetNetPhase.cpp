// TailnetNetPhase.h: the framebuffer goes back to the heap while Tailscale runs.
//
// Why free + allocate again, not FreeInkDisplay's lend (lendBuildStorage): a
// lent block can only serve code that is handed its address. The memory
// Tailscale needs is taken by WiFi, lwIP, MicroLink (context, task stacks,
// mbedTLS) and BookOrbit's wolfSSL, all from the system heap, so only a free
// puts the 52 KB where they can use it. The fragmentation HalDisplay.h warns
// about comes from repeated free+realloc cycles over a long session; here there
// is one cycle per boot, and every one of these boots ends in a restart or deep
// sleep. The block is asked for again only once WiFi is off and MicroLink is
// stopped and destroyed (the shim's worker calls tailnet_display_unpark()), when
// the memory it lent is free again. If the heap still cannot give 52 KB in one
// block, nothing is drawn (the panel keeps its image), the error is logged and
// the render task keeps trying.
#include "TailnetNetPhase.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <esp_heap_caps.h>
#include <Logging.h>

#include <atomic>
#include <new>

#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "components/UITheme.h"
#include "Tailnet.h"
#include "TailnetSysSwitches.h"
#include "SilentRestart.h"
#include "activities/browser/BookOrbitCatalogLink.h"
#include "activities/browser/BookOrbitCatalogListCache.h"
#include <CrossInkTailnetCuts.h>
#include "network/SavedWifiConnect.h"

extern GfxRenderer renderer;  // main.cpp

namespace {

// NetworkBootTarget values (SilentRestart.h) whose screens need no answer while
// WiFi is up: BOOKORBIT_SYNC, BOOKORBIT_SLEEP_SYNC, BOOKORBIT_DOWNLOAD (a
// static "Downloading" screen, see tailnet_boot_parks()), BOOKORBIT_HOME_REFRESH,
// BOOKORBIT_HOME_SHELVES (it fetches, turns WiFi off, then shows the pickers),
// KOREADER_SYNC and KOREADER_AUTH (on demand, below; only in a build with KOReader
// Sync), and the overlay's own login and BookOrbit authentication boots. (A sync that
// asks which position to keep is covered by the tick's fallback.) Only these boots run
// Tailscale on the C3. By name, never by number: CrossInk v2 renumbered them (the
// BookOrbit ones 8-13 -> 32-37) and gave 8 and 9 to asset downloads and plugins, which
// never park nor start Tailscale.
constexpr uint32_t targetValue(NetworkBootTarget target) { return static_cast<uint32_t>(target); }
constexpr uint32_t CATALOG_TARGET = targetValue(NetworkBootTarget::BOOKORBIT_CATALOG);

// KOReader's server is usually on the internet; Tailscale starts only when the
// sync resolves a tailnet name or connects to a tailnet address.
// CrossInk v2 builds without KOReader Sync (CROSSINK_APP_CAP_KOREADER_SYNC=0) still
// upload to an existing KOReader account: those uploads run in the overlay's own boot.
bool onDemand(uint32_t target) {
  if (target == TAILNET_KOREADER_UPLOAD_TARGET) return true;
#if CROSSINK_APP_CAP_KOREADER_SYNC
  return target == targetValue(NetworkBootTarget::KOREADER_SYNC) ||
         target == targetValue(NetworkBootTarget::KOREADER_AUTH);
#else
  return false;
#endif
}

// The boots that take the parked WiFi buffer tier (TailnetWifiMem.c): every parking
// boot but the on-demand ones.
bool parkedTier(uint32_t target) {
  return target == targetValue(NetworkBootTarget::BOOKORBIT_SYNC) ||
         target == targetValue(NetworkBootTarget::BOOKORBIT_SLEEP_SYNC) ||
         target == targetValue(NetworkBootTarget::BOOKORBIT_DOWNLOAD) ||
         target == targetValue(NetworkBootTarget::BOOKORBIT_HOME_REFRESH) ||
         target == targetValue(NetworkBootTarget::BOOKORBIT_HOME_SHELVES) || target == TAILNET_LOGIN_TARGET ||
         target == TAILNET_BOOKORBIT_AUTH_TARGET;
}

bool parksFor(uint32_t target) { return parkedTier(target) || onDemand(target); }

std::atomic<bool> s_armed{false};
std::atomic<bool> s_parked{false};
std::atomic<bool> s_net_down{false};  // WiFi went down after the park
std::atomic<bool> s_pending{false};   // a render was skipped
uint32_t s_last_tick = 0;             // main task only
uint32_t s_idle_since = 0;
bool s_escalated = false;
std::atomic<bool> s_prompt_dropped{false};  // tailnet_prompt_dropped_network()
// TAILNET_PARK_DURING_CONNECT: the live catalog boot may park while a control
// session runs (tailnet_wifi_joined(), catalogConnect()).
std::atomic<bool> s_connect_park{false};
uint32_t s_target = 0;                   // this boot's NetworkBootTarget value
std::atomic<uint32_t> s_down_since{0};   // millis() when WiFi went down while parked
std::atomic<bool> s_restarting{false};   // a restart is under way: draw nothing
// No screen can come back within this long after the network went down while
// the framebuffer is parked: restart instead of waiting forever.
constexpr uint32_t UNPARK_GIVE_UP_MS = 15000;

// The live catalog's restarts (TAILNET_PARK_DURING_CONNECT), carried across a
// silent restart into BOOKORBIT_CATALOG and consumed by the next
// tailnet_network_phase_begin(). Each restart kind happens at most once per
// catalog open, so the sequence is bounded.
RTC_NOINIT_ATTR uint32_t s_rtc_magic;
RTC_NOINIT_ATTR uint32_t s_rtc_flags;
constexpr uint32_t CATALOG_RTC_MAGIC = 0x7A1C0A11u;
constexpr uint32_t CATALOG_AFTER_CONTROL = 1u << 0;  // the last boot ran the control session and cached its state
constexpr uint32_t CATALOG_RECOVERED = 1u << 1;      // the last boot failed and restarted once
constexpr uint32_t CATALOG_NO_TAILSCALE = 1u << 2;   // two failures: no Tailscale; the catalog shows its error
uint32_t s_catalog_flags = 0;                        // this boot's
// The link the BookOrbit Home opened the catalog with: the catalog takes it off
// the SD card, so a restart puts it back.
BookOrbitCatalogLink* s_entry_link = nullptr;
#if CROSSINK_TAILNET_DOWNLOAD_BACK_TO_CATALOG
// The listing a download boot sent this catalog boot back to (saveCatalogReturn()):
// the catalog takes it off the card too, so a restart puts it back as well.
BookOrbitDownloadResume* s_return_listing = nullptr;
#endif

// Caller holds the RenderLock.
bool unparkLocked() {
  if (!s_parked.load()) return true;
  if (!display.reallocFrameBuffer()) {
    LOG_ERR("TNET", "Framebuffer not back yet: free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return false;
  }
  renderer.restoreFrameBufferAfterBuild();  // points the renderer at it again
  s_parked = false;
  LOG_INF("TNET", "Framebuffer back: free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  return true;
}

void dropEntryLink() {
  delete s_entry_link;
  s_entry_link = nullptr;
#if CROSSINK_TAILNET_DOWNLOAD_BACK_TO_CATALOG
  delete s_return_listing;
  s_return_listing = nullptr;
#endif
}

// Restarts into the live catalog with `flags` for the next boot. Does not return.
void restartCatalog(uint32_t flags, const char* why) {
  LOG_INF("TNET", "Live catalog: restarting into the catalog (%s; flags %lu)", why, static_cast<unsigned long>(flags));
  if (s_entry_link) BookOrbitCatalogListCache::saveEntryLink(*s_entry_link);
#if CROSSINK_TAILNET_DOWNLOAD_BACK_TO_CATALOG
  if (s_return_listing) BookOrbitCatalogListCache::saveCatalogReturn(*s_return_listing);
#endif
  s_rtc_flags = flags;
  s_rtc_magic = CATALOG_RTC_MAGIC;
  s_restarting = true;
  silentRestartToNetwork(NetworkBootTarget::BOOKORBIT_CATALOG);
  ESP.restart();  // not reached (silentRestartToNetwork returns only while entering deep sleep)
}

// The framebuffer cannot come back. In the live catalog: one restart into it
// (a fresh heap), then one more without Tailscale, where the catalog's own
// error screen shows (Back goes Home). Elsewhere: Home.
void giveUpOnFramebuffer() {
#if TAILNET_PARK_DURING_CONNECT
  if (s_target == CATALOG_TARGET && tailnet_lite()) {
    if (!(s_catalog_flags & CATALOG_RECOVERED)) restartCatalog(s_catalog_flags | CATALOG_RECOVERED, "framebuffer lost");
    restartCatalog(s_catalog_flags | CATALOG_RECOVERED | CATALOG_NO_TAILSCALE, "framebuffer lost twice");
  }
#endif
  s_restarting = true;
  silentRestart();
  ESP.restart();
}

}  // namespace

extern "C" void tailnet_network_phase_begin(uint32_t target) {
  s_target = target;
#if TAILNET_PARK_DURING_CONNECT
  const uint32_t flags = s_rtc_magic == CATALOG_RTC_MAGIC ? s_rtc_flags : 0;
  s_rtc_magic = 0;  // consumed: a fresh catalog open starts with none
  if (tailnet_lite() && target == CATALOG_TARGET) {  // see catalogConnect()
    s_catalog_flags = flags;
    s_connect_park = true;
    // Keep a copy of the Home's link (the catalog takes it off the card).
    BookOrbitCatalogLink link;
    if (BookOrbitCatalogListCache::takeEntryLink(link)) {
      BookOrbitCatalogListCache::saveEntryLink(link);
      s_entry_link = new (std::nothrow) BookOrbitCatalogLink(std::move(link));
    }
#if CROSSINK_TAILNET_DOWNLOAD_BACK_TO_CATALOG
    BookOrbitDownloadResume listing;
    if (BookOrbitCatalogListCache::takeCatalogReturn(listing)) {
      BookOrbitCatalogListCache::saveCatalogReturn(listing);
      s_return_listing = new (std::nothrow) BookOrbitDownloadResume(std::move(listing));
    }
#endif
    LOG_INF("TNET", "Live catalog boot: flags %lu (after control %d, recovered %d, no Tailscale %d)",
            static_cast<unsigned long>(flags), (flags & CATALOG_AFTER_CONTROL) != 0, (flags & CATALOG_RECOVERED) != 0,
            (flags & CATALOG_NO_TAILSCALE) != 0);
    return;
  }
#endif
#if TAILNET_PROBE_CATALOG_LIVE
  if (tailnet_lite() && target == CATALOG_TARGET) {  // screen kept
    tailnet_probe_permit_unparked();
    return;
  }
#endif
  // The S3 readers keep the framebuffer in PSRAM and run Tailscale anywhere.
  if (!tailnet_lite() || !parksFor(target)) return;
  s_armed = true;
  if (onDemand(target)) {
    tailnet_permit_on_demand();
  } else {
    tailnet_permit_start(target == TAILNET_LOGIN_TARGET);
  }
}

extern "C" uint32_t tailnet_network_boot_target(void) { return s_target; }

extern "C" bool tailnet_wifi_parked_boot(uint32_t target) { return parkedTier(target); }

extern "C" bool tailnet_koreader_upload_redirect(void) {
  if (!tailnet_lite() || !tailnet_enabled() || !tailnet_joined()) return false;
  return s_target != TAILNET_KOREADER_UPLOAD_TARGET;
}

extern "C" bool tailnet_boot_runs_tailscale(void) {
  return tailnet_lite() && tailnet_enabled() && (parksFor(s_target) || s_target == CATALOG_TARGET);
}

extern "C" bool tailnet_prompt_dropped_network(void) { return s_prompt_dropped.load(); }

extern "C" bool tailnet_boot_parks(void) { return s_armed.load() && tailnet_enabled() && tailnet_joined(); }

extern "C" bool tailnet_end_parked_network(void) {
  if (!tailnet_boot_parks()) return false;
  tailnet_wait_idle(10000);  // never cut a control session off mid-exchange
  SavedWifiConnect::disconnect();
  if (!tailnet_display_ready(10000)) LOG_ERR("TNET", "Framebuffer still parked after the network was turned off");
  return true;
}

extern "C" bool tailnet_display_park(void) {
  if (!s_armed.load() && !s_connect_park.load()) return false;
  if (s_parked.load()) return true;
  RenderLock lock;  // not while a render draws into it
  // Lent to a layout build (FrameBufferLoan): the bytes are in use; keep it.
  if (!renderer.hasFrameBuffer()) {
    LOG_ERR("TNET", "Framebuffer lent to a build; not parking it");
    return false;
  }
  const uint32_t before = ESP.getFreeHeap();
  display.releaseFrameBuffer();
  renderer.releaseFrameBufferForBuild();  // nothing left to lend: just clears the renderer's pointer
  s_net_down = false;
  s_down_since = 0;
  s_escalated = false;
  s_parked = true;
  LOG_INF("TNET", "Framebuffer parked for Tailscale: free %u -> %u maxAlloc=%u", before, ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
  return true;
}

extern "C" void tailnet_display_unpark(void) {
  if (!s_parked.load()) return;
  if (!s_net_down.exchange(true)) s_down_since = millis() | 1u;
  bool back;
  {
    RenderLock lock;
    back = unparkLocked();
  }
  if (back && s_pending.exchange(false)) activityManager.requestUpdate(true);
}

extern "C" bool tailnet_display_render_gate(void) {
  // The render task holds the RenderLock here.
  if (!s_parked.load() || (s_net_down.load() && unparkLocked())) return true;
  s_pending = true;
  return false;
}

extern "C" void tailnet_display_tick(void) {
  if (!s_parked.load()) return;
  const uint32_t now = millis();
  if (now - s_last_tick > 250) s_idle_since = now;  // the main task was busy in between
  s_last_tick = now;
  if (s_net_down.load()) {
    const uint32_t since = s_down_since.load();
    if (since && now - since >= UNPARK_GIVE_UP_MS) {
      // Never hang on a parked framebuffer: the heap cannot give it back.
      LOG_ERR("TNET", "Framebuffer not back %lu ms after the network went down (free=%u maxAlloc=%u); restarting",
              static_cast<unsigned long>(now - since), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      giveUpOnFramebuffer();
      return;
    }
  }
  if (!s_pending.load()) return;
  if (s_net_down.load()) {
    if (now - s_idle_since >= 250) {
      s_idle_since = now;
      activityManager.requestUpdate(true);  // the gate tries the framebuffer again
    }
    return;
  }
  if (!s_escalated && now - s_idle_since >= 2000) {
    s_escalated = true;
#if CROSSINK_TAILNET_PROMPT_KEEPS_NETWORK
    // Take the framebuffer back beside Tailscale when the heap has the block (a sync's
    // cached start leaves ~77 KB): the prompt's choice then runs over the kept connection.
    const uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    bool back;
    {
      RenderLock lock;
      back = unparkLocked();
    }
    if (back) {
      LOG_INF("TNET", "Prompt with the network kept (largest %u)", static_cast<unsigned>(largest));
      if (s_pending.exchange(false)) activityManager.requestUpdate(true);
      return;
    }
    s_prompt_dropped = true;
    LOG_ERR("TNET", "Prompt: the framebuffer does not fit beside Tailscale (largest %u); turning WiFi off",
            static_cast<unsigned>(largest));
#endif
    LOG_ERR("TNET", "A screen waits for input while Tailscale holds the framebuffer's memory; turning WiFi off");
    SavedWifiConnect::disconnect();
  }
}

extern "C" bool tailnet_download_progress_unpark(void) {
#if CROSSINK_TAILNET_DOWNLOAD_PROGRESS
  if (!s_parked.load() || s_net_down.load() || tailnet_status() != TAILNET_CONNECTED) return false;
  // The framebuffer plus the download's TLS handshake and full-record buffer (~22 KB on
  // the X3) must fit beside Tailscale: ask for the framebuffer only with that margin left.
  const uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const uint32_t wanted = display.getBufferSize() + CROSSINK_TAILNET_DOWNLOAD_PROGRESS_MARGIN;
  bool back = false;
  if (largest >= wanted) {
    RenderLock lock;
    back = unparkLocked();
  }
  if (!back) {
    LOG_INF("TNET", "Download progress UI: stays parked (largest %u, wanted %u)", static_cast<unsigned>(largest),
            static_cast<unsigned>(wanted));
    return false;
  }
  LOG_INF("TNET", "Download progress UI: framebuffer back with the network (largest %u)",
          static_cast<unsigned>(largest));
  if (s_pending.exchange(false)) activityManager.requestUpdate(true);
  return true;
#else
  return false;
#endif
}

extern "C" bool tailnet_display_ready(uint32_t timeout_ms) {
  if (s_restarting.load()) return !s_parked.load();  // a restart's popup: never wait for it
  const uint32_t start = millis();
  while (s_parked.load()) {
    if (s_net_down.load()) {
      RenderLock lock(RenderLock::Mode::Try);
      if (lock.ownsLock() && unparkLocked()) return true;
    }
    if (millis() - start >= timeout_ms) {
      LOG_ERR("TNET", "Framebuffer still parked after %lu ms; not drawing", static_cast<unsigned long>(timeout_ms));
      return false;
    }
    delay(50);
  }
  return true;
}

#if TAILNET_PARK_DURING_CONNECT
namespace {
// The live catalog boot on the C3. A control session with the framebuffer kept
// takes the heap down to ~4 KB, and one run parked leaves long-lived blocks in
// the framebuffer's hole, so the framebuffer never comes back in one piece. So:
//  - usable cached control state: no control session at all and no park; the
//    catalog runs on the same heap the probe measured (the shim's decision);
//  - otherwise: park, run the full control session, and restart into the
//    catalog once its state is cached: the next boot takes the cached path on
//    a clean heap. Never a second control session in a row.
// Every failure ends in a bounded restart (giveUpOnFramebuffer()) or, with the
// framebuffer kept, in the catalog's own error screen with the network off, so
// its requests fail at once.
void catalogConnect() {
  const uint32_t flags = s_catalog_flags;
  if (!tailnet_enabled() || !tailnet_joined()) {
    s_connect_park = false;  // no Tailscale in this boot
    dropEntryLink();
    return;
  }
  if (flags & CATALOG_NO_TAILSCALE) {
    LOG_ERR("TNET", "Live catalog: Tailscale failed twice; not starting it (the catalog shows its error)");
    s_connect_park = false;
    dropEntryLink();
    SavedWifiConnect::disconnect();  // requests fail at once
    return;
  }
  {
    RenderLock lock;
    if (renderer.hasFrameBuffer()) GUI.drawPopup(renderer, "Connecting to Tailscale...");
  }
  LOG_INF("TNET", "Live catalog: connecting: free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  // A control session right after one would only repeat what failed.
  tailnet_catalog_connect(!(flags & CATALOG_AFTER_CONTROL));
  const bool ready = tailnet_wait_ready(20000);
  const int result = tailnet_catalog_result();
  if (tailnet_status() == TAILNET_CONNECTED) tailnet_stack_report("Tailscale ready");
  LOG_INF("TNET", "Live catalog: %s (result %d, %s): free=%u maxAlloc=%u",
          result == TAILNET_CATALOG_CACHED    ? "cached start"
          : result == TAILNET_CATALOG_CONTROL ? "control session, parked"
          : result == TAILNET_CATALOG_REFUSED ? "refused (no usable cached state)"
                                              : "not started",
          result, ready && tailnet_status() == TAILNET_CONNECTED ? "ready" : "not ready", ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
  if (result == TAILNET_CATALOG_CONTROL || s_parked.load()) {
    // Never take the framebuffer back on the heap a control session left.
    tailnet_wait_idle(10000);  // the session's NVS writes finish first
    restartCatalog(flags | CATALOG_AFTER_CONTROL,
                   tailnet_status() == TAILNET_CONNECTED ? "control state cached" : "control session did not finish");
  }
  s_connect_park = false;
  if (result == TAILNET_CATALOG_CACHED && ready && tailnet_status() == TAILNET_CONNECTED) {
    dropEntryLink();
    return;
  }
  // The cached state did not work (MicroLink erased it) or nothing started. A
  // restart can run the control session parked, once; otherwise the catalog
  // shows its error with the network off.
  if (!(flags & (CATALOG_AFTER_CONTROL | CATALOG_RECOVERED))) {
    restartCatalog(flags | CATALOG_RECOVERED, "cached start failed");
  }
  LOG_ERR("TNET", "Live catalog: no Tailscale this boot; turning WiFi off so requests fail at once");
  dropEntryLink();
  SavedWifiConnect::disconnect();
}
}  // namespace
#endif

extern "C" void tailnet_wifi_joined(void) {
  tailnet_stack_report("WiFi joined");  // TailnetStacks.c (TAILNET_STACK_LOG)
#if TAILNET_PARK_DURING_CONNECT
  if (s_connect_park.load()) {
    catalogConnect();
    return;
  }
#endif
  // Returns at once unless this boot runs Tailscale (tailnet_permit_start()).
  tailnet_wait_ready(20000);
  if (tailnet_status() == TAILNET_CONNECTED) tailnet_stack_report("Tailscale ready");
}
