// The display framebuffer goes back to the heap while Tailscale runs (ESP32-C3).
// Lite builds start Tailscale only in these boots (Tailnet.h, tailnet_lite()).
//
// On the X3 (ESP32-C3, no PSRAM) CrossInk keeps one ~52 KB framebuffer (48 KB on
// the X4) in internal RAM for the whole boot. In the minimal network boots that
// draw nothing the user must answer while WiFi is up (BookOrbit sync, the sync
// behind the sleep screen, the BookOrbit Home refresh and shelves, the login
// and BookOrbit authentication boots), it is
// freed before MicroLink starts and allocated again once WiFi is off and
// MicroLink has freed its memory. The e-ink panel keeps its last image unpowered meanwhile; screens
// asked for in between are drawn once the framebuffer is back.
//
// Implemented in tailnet/ui/TailnetNetPhase.cpp (it needs CrossInk's renderer);
// CrossInk calls the boot functions above and the next four; the shim's worker
// calls the last two.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The overlay's own network boot (a NetworkBootTarget value well clear of
// upstream's): Settings > Tailscale > Log in on the C3 (TailnetLoginActivity).
#define TAILNET_LOGIN_TARGET 64u

// main.cpp, in that boot: starts the login screen. False if it could not.
bool tailnet_login_boot(void);

// The same for Settings > BookOrbit > Authenticate (TailnetBookOrbitAuthActivity):
// CrossInk runs it in the normal UI, which cannot hold Tailscale on the C3.
#define TAILNET_BOOKORBIT_AUTH_TARGET 65u
// ActivityManager::goToBookOrbitAuth(): true when it restarted into that boot
// (lite build, Tailscale on and logged in); false runs CrossInk's own screen.
bool tailnet_redirect_bookorbit_auth(void);
bool tailnet_bookorbit_auth_boot(void);

// KOReader-server uploads (CrossInk v2: progress, reading stats and clippings for an
// existing KOReader account, StatsUploadActivity) on the C3: the server may be on the
// tailnet, which the normal UI cannot reach. CrossInk's patch restarts the upload into
// this boot, where Tailscale starts on demand (only when the server's name or address
// is a tailnet one) and the framebuffer is parked while it runs.
#define TAILNET_KOREADER_UPLOAD_TARGET 66u
// True when the upload must restart into that boot (lite build, Tailscale on and
// logged in, not already in it); false runs it where it is, as upstream does.
bool tailnet_koreader_upload_redirect(void);

// CrossInk, right after WiFi joins (SavedWifiConnect and WifiSelectionActivity):
// waits for Tailscale in the boots that run it (tailnet_wait_ready()), so the
// first request's heap check and connect see it ready. No-op elsewhere.
void tailnet_wifi_joined(void);

// True when this boot runs Tailscale with the framebuffer parked (lite build,
// a parking boot, Tailscale on and logged in): screens must be drawn before
// WiFi joins or after tailnet_end_parked_network().
bool tailnet_boot_parks(void);
// In such a boot: waits for a running control session, turns WiFi off and
// gets the framebuffer back. False (and nothing done) in any other boot.
bool tailnet_end_parked_network(void);

// main.cpp, once the minimal network boot is ready. Arms parking for the boots
// listed above (target is a NetworkBootTarget value), on the C3 only.
void tailnet_network_phase_begin(uint32_t target);
// This boot's network boot target as given to tailnet_network_phase_begin(); 0 in
// any other boot. TailnetWifiMem.c picks the WiFi buffer tier from it.
uint32_t tailnet_network_boot_target(void);
// True when `target` takes the parked WiFi buffer tier (TailnetWifiMem.c). The
// NetworkBootTarget numbers differ between CrossInk versions, so only the UI side,
// which compiles against SilentRestart.h, knows them.
bool tailnet_wifi_parked_boot(uint32_t target);
// True in a boot that runs Tailscale on the C3 (a parking boot or the live catalog,
// lite build, Tailscale on). CrossInk skips plugin event delivery there.
bool tailnet_boot_runs_tailscale(void);
// True once a screen that waited for input made this boot turn WiFi off to get the
// framebuffer back (CROSSINK_TAILNET_PROMPT_KEEPS_NETWORK found no room for it beside
// Tailscale): a choice that needs the network must restart instead of failing offline.
bool tailnet_prompt_dropped_network(void);

// CROSSINK_TAILNET_DOWNLOAD_PROGRESS, main task, in a parked download boot once Tailscale
// is ready: takes the framebuffer back with WiFi and Tailscale kept when the largest free
// block holds it plus CROSSINK_TAILNET_DOWNLOAD_PROGRESS_MARGIN, so the download shows its
// progress bar. False: it stays parked (the static "Downloading" screen stays on the panel).
bool tailnet_download_progress_unpark(void);

// Render task, before an activity renders. False: the framebuffer is parked,
// skip this render (it is drawn once the framebuffer is back).
bool tailnet_display_render_gate(void);

// Main task, every ActivityManager::loop(). If a screen has been waiting for the
// framebuffer for 2 s while the main loop idles (a screen expecting input with
// Tailscale still up), turns WiFi off so the framebuffer can come back.
void tailnet_display_tick(void);

// Main task, before drawing outside the render task. Waits up to timeout_ms for
// the framebuffer; false means it is still parked and nothing may be drawn.
bool tailnet_display_ready(uint32_t timeout_ms);

// Shim worker: before MicroLink starts / after WiFi is down and MicroLink freed.
// park() is false when this boot does not park (MicroLink then stays off on
// the C3). Weak, so builds without the UI side still link.
bool tailnet_display_park(void) __attribute__((weak));
void tailnet_display_unpark(void) __attribute__((weak));

// Live catalog connect (TAILNET_PARK_DURING_CONNECT, ui/TailnetNetPhase.cpp):
// starts Tailscale, and the worker decides how. Usable cached control state:
// no control session, the framebuffer kept (TAILNET_CATALOG_CACHED). Otherwise,
// if allow_control, the framebuffer is parked for a full control session
// (TAILNET_CATALOG_CONTROL; the caller then restarts the boot rather than
// taking the framebuffer back on a fragmented heap); else nothing starts
// (TAILNET_CATALOG_REFUSED).
enum { TAILNET_CATALOG_NONE = 0, TAILNET_CATALOG_ALLOW_CONTROL = 1, TAILNET_CATALOG_CACHED_ONLY = 2 };
enum {
  TAILNET_CATALOG_PENDING = 0,
  TAILNET_CATALOG_CACHED = 1,
  TAILNET_CATALOG_CONTROL = 2,
  TAILNET_CATALOG_REFUSED = 3,
  TAILNET_CATALOG_FAILED = 4,
};
void tailnet_catalog_connect(bool allow_control);
int tailnet_catalog_result(void);

#ifdef __cplusplus
}
#endif
