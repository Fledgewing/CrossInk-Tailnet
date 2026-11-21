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
#include <Logging.h>

#include <atomic>

#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "Tailnet.h"
#include "network/SavedWifiConnect.h"

extern GfxRenderer renderer;  // main.cpp

namespace {

// NetworkBootTarget values (SilentRestart.h) whose screens need no answer while
// WiFi is up: BOOKORBIT_SYNC, BOOKORBIT_SLEEP_SYNC, BOOKORBIT_HOME_REFRESH,
// BOOKORBIT_HOME_SHELVES (it fetches, turns WiFi off, then shows the pickers),
// and the overlay's own login and BookOrbit authentication boots. (A sync that
// asks which position to keep is covered by the tick's fallback.) Only these
// boots run Tailscale on the C3.
bool parksFor(uint32_t target) {
  return target == 8 || target == 9 || target == 12 || target == 13 || target == TAILNET_LOGIN_TARGET ||
         target == TAILNET_BOOKORBIT_AUTH_TARGET;
}

std::atomic<bool> s_armed{false};
std::atomic<bool> s_parked{false};
std::atomic<bool> s_net_down{false};  // WiFi went down after the park
std::atomic<bool> s_pending{false};   // a render was skipped
uint32_t s_last_tick = 0;             // main task only
uint32_t s_idle_since = 0;
bool s_escalated = false;

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

}  // namespace

extern "C" void tailnet_network_phase_begin(uint32_t target) {
#if TAILNET_PROBE_CATALOG_LIVE
  if (tailnet_lite() && target == 11) {  // BOOKORBIT_CATALOG, screen kept
    tailnet_probe_permit_unparked();
    return;
  }
#endif
  // The S3 readers keep the framebuffer in PSRAM and run Tailscale anywhere.
  if (!tailnet_lite() || !parksFor(target)) return;
  s_armed = true;
  tailnet_permit_start(target == TAILNET_LOGIN_TARGET);
}

extern "C" bool tailnet_display_park(void) {
  if (!s_armed.load()) return false;
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
  s_escalated = false;
  s_parked = true;
  LOG_INF("TNET", "Framebuffer parked for Tailscale: free %u -> %u maxAlloc=%u", before, ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
  return true;
}

extern "C" void tailnet_display_unpark(void) {
  if (!s_parked.load()) return;
  s_net_down = true;
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
    LOG_ERR("TNET", "A screen waits for input while Tailscale holds the framebuffer's memory; turning WiFi off");
    SavedWifiConnect::disconnect();
  }
}

extern "C" bool tailnet_display_ready(uint32_t timeout_ms) {
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

extern "C" void tailnet_wifi_joined(void) {
  // Returns at once unless this boot runs Tailscale (tailnet_permit_start()).
  tailnet_wait_ready(20000);
}
