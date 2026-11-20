// Tailnet: Tailscale (via MicroLink) layered onto CrossInk without editing it.
//
// The service starts MicroLink whenever the reader's WiFi station gets an IP
// address and stops it when WiFi goes away, so every screen that turns WiFi on
// (BookOrbit sync, catalog, OPDS, OTA, web server) gets the tailnet for free.
// It installs itself from a static constructor; CrossInk calls nothing.
//
// The functions below are for optional UI (the login QR screen). They are
// safe to call from any task at any time.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  TAILNET_OFF = 0,       // WiFi is down, or the service is disabled
  TAILNET_STARTING,      // contacting the control plane
  TAILNET_NEEDS_LOGIN,   // tailnet_login_url() has the URL to show
  TAILNET_CONNECTED,     // has a 100.x address; tailnet hosts are reachable
  TAILNET_RECONNECTING,
  TAILNET_FAILED,        // auth rejected or a hard error; see the serial log
} tailnet_status_t;

tailnet_status_t tailnet_status(void);

// Buffer size that always holds a login URL (MicroLink's MICROLINK_AUTH_URL_MAX).
#define TAILNET_LOGIN_URL_MAX 256

// Copies the interactive login URL into buf. Returns false when there is none
// (status is not TAILNET_NEEDS_LOGIN).
bool tailnet_login_url(char* buf, size_t len);

// The reader's tailnet IPv4 address in host byte order, or 0.
uint32_t tailnet_ip(void);

// Bumped on every status change, so a screen can redraw only when needed.
uint32_t tailnet_generation(void);

// Persisted on/off switch (NVS), off by default. Off stops MicroLink and keeps
// it stopped; on starts it whenever WiFi is up.
bool tailnet_enabled(void);
void tailnet_set_enabled(bool enabled);

// True once this reader has connected to a tailnet, until tailnet_logout().
bool tailnet_joined(void);

// Forget this reader's tailnet identity (keys and cached peers). The next
// start asks for a fresh login.
void tailnet_logout(void);

// Block until the tailnet is connected, while it is starting or reconnecting.
// Returns false at once when it is off, failed or waiting for a login.
bool tailnet_wait_connected(uint32_t timeout_ms);

// Make sure MicroLink tracks the peer at ip (host byte order). On a tailnet
// larger than its peer table, a peer can be left out; this pins it as the
// priority peer (remembered in NVS) and restarts MicroLink so it is added.
// Tried once per peer per WiFi session. Blocks up to about 2 x timeout_ms.
bool tailnet_ensure_peer(uint32_t ip, uint32_t timeout_ms);

// Resolve a tailnet name ("bookorbit", "bookorbit.tail1234.ts.net") to an IPv4
// address in host byte order via MicroLink's peer list. 0 if unknown.
uint32_t tailnet_resolve(const char* name);

#ifdef __cplusplus
}
#endif
