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

// main.cpp, once the minimal network boot is ready. Arms parking for the boots
// listed above (target is a NetworkBootTarget value), on the C3 only.
void tailnet_network_phase_begin(uint32_t target);

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

#ifdef __cplusplus
}
#endif
