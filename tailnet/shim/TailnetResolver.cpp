// Tailnet names and addresses for the whole firmware, without MagicDNS.
//
// Every resolver path in CrossInk ends in lwip_getaddrinfo(), and every TCP
// client in lwip_connect(): Arduino's NetworkClient and HTTPClient, the wolfSSL
// SecureHttpClient, and esp-tls (esp_http_client: OTA and downloads).
// tailnet_build.py links with --wrap for both, so callers in the prebuilt
// libraries land here.
//
// Names: a peer MicroLink knows ("bookorbit", "bookorbit.tail1234.ts.net") is
// answered by passing its 100.x address to the real lwip_getaddrinfo as a
// numeric host, which builds (and lwip_freeaddrinfo later frees) the result the
// usual way. Anything else goes to DNS untouched.
//
// Timing: CrossInk starts network work (a BookOrbit sync) the moment WiFi has
// an address, while the tailnet needs a few seconds more. Lookups, and connects
// to tailnet addresses, wait for it while it is starting (up to WAIT_MS).
//
// Large tailnets: MicroLink tracks a limited number of peers. A connect to a
// tailnet address it is not tracking pins that address as its priority peer
// and restarts it, so the server is added (see tailnet_ensure_peer).
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "Tailnet.h"

namespace {
constexpr uint32_t WAIT_MS = 20000;

// Names worth waiting for: single-label names ("bookorbit") and MagicDNS names
// ("bookorbit.tail1234.ts.net"). Public names (api.github.com, and MicroLink's
// own controlplane.tailscale.com and DERP lookups) never wait.
bool mayBeTailnetName(const char* name) {
  const size_t n = strlen(name);
  const size_t m = n && name[n - 1] == '.' ? n - 1 : n;  // ignore a trailing dot
  if (!memchr(name, '.', m)) return true;
  static constexpr char kSuffix[] = ".ts.net";
  constexpr size_t k = sizeof(kSuffix) - 1;
  return m > k && strncasecmp(name + m - k, kSuffix, k) == 0;
}

// Tailscale's IPv4 range, 100.64.0.0/10 (host byte order).
bool isTailnetAddr(uint32_t ip) { return (ip & 0xFFC00000u) == 0x64400000u; }
}  // namespace

extern "C" int __real_lwip_getaddrinfo(const char* nodename, const char* servname, const struct addrinfo* hints,
                                       struct addrinfo** res);
extern "C" int __real_lwip_connect(int s, const struct sockaddr* name, socklen_t namelen);

extern "C" int __wrap_lwip_getaddrinfo(const char* nodename, const char* servname, const struct addrinfo* hints,
                                       struct addrinfo** res) {
  const bool v4_ok = !hints || hints->ai_family == AF_UNSPEC || hints->ai_family == AF_INET;
  const bool numeric_only = hints && (hints->ai_flags & AI_NUMERICHOST);
  ip4_addr_t probe;
  if (nodename && v4_ok && !numeric_only && !ip4addr_aton(nodename, &probe)) {
    uint32_t ip = tailnet_resolve(nodename);
    if (!ip && mayBeTailnetName(nodename) && tailnet_wait_connected(WAIT_MS)) ip = tailnet_resolve(nodename);
    if (ip) {
      char lit[16];
      snprintf(lit, sizeof(lit), "%u.%u.%u.%u", (unsigned)(ip >> 24) & 0xff, (unsigned)(ip >> 16) & 0xff,
               (unsigned)(ip >> 8) & 0xff, (unsigned)ip & 0xff);
      return __real_lwip_getaddrinfo(lit, servname, hints, res);
    }
  }
  return __real_lwip_getaddrinfo(nodename, servname, hints, res);
}

extern "C" int __wrap_lwip_connect(int s, const struct sockaddr* name, socklen_t namelen) {
  if (name && name->sa_family == AF_INET && namelen >= sizeof(struct sockaddr_in)) {
    const uint32_t ip = lwip_ntohl(reinterpret_cast<const struct sockaddr_in*>(name)->sin_addr.s_addr);
    if (isTailnetAddr(ip) && tailnet_enabled() && !tailnet_ensure_peer(ip, WAIT_MS) &&
        tailnet_status() == TAILNET_CONNECTED) {
      // Connected, but this address is not a peer: MicroLink would drop the
      // packets and the caller would wait out its whole timeout. Fail now.
      errno = EHOSTUNREACH;
      return -1;
    }
  }
  return __real_lwip_connect(s, name, namelen);
}
