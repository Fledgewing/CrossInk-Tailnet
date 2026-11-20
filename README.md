# CrossInk-Tailnet

[CrossInk-Bookorbit](https://github.com/agosez/CrossInk-Bookorbit) firmware with your reader on
your Tailscale tailnet, using [MicroLink](https://github.com/CamM2325/microlink). BookOrbit sync,
the catalog, OPDS and downloads can reach servers that are only on your tailnet.

This is an **overlay**, not a fork. CrossInk's source is used as released; the overlay adds:

| Piece | Where | What it does |
|---|---|---|
| MicroLink | `third_party/microlink` (submodule, pinned tag) | Tailscale-compatible WireGuard client for ESP32 |
| Shim | `tailnet/shim` | Starts MicroLink when WiFi gets an address, stops it when WiFi goes away; answers tailnet names (`bookorbit`, `bookorbit.<tailnet>.ts.net`) for every HTTP client in the firmware |
| Settings screen | `tailnet/ui` | **Settings > Tailscale**: on/off, status and address, log in (QR code) and log out |
| Build hook | `tailnet/tailnet_build.py` + `overlay/platformio.local.ini` | Compiles the above into CrossInk's normal PlatformIO build and points the updater at this repo's releases |
| Settings hook | `overlay/patches/0001-tailscale-settings-entry.patch` | Seven lines adding the Settings row: the only edit to CrossInk's source |
| MicroLink patch | `overlay/patches-microlink/0001-relay-regions.patch` | Closest relay region and per-peer relays (below), applied to the pinned MicroLink |

When CrossInk-Bookorbit releases, the daily CI run rebuilds the new tag with the overlay and
publishes a release here. The only thing that can need hand work is the small settings patch, and
CI says so if it stops applying.

## Readers

| Reader | Env | Notes |
|---|---|---|
| X4 Pro | `x4-pro` | ESP32-S3 + PSRAM. Primary target. |
| X4 Classic | `x4-classic` | ESP32-S3 + PSRAM. |
| X3 / X4 | `default` | ESP32-C3, no PSRAM. Built with English UI only to make room; smaller MicroLink buffers. |

The partition table is never changed (that can only be done over USB).

## First install

Flash the `.bin` for your reader from the latest release the same way you install CrossInk from
an SD card or USB. After that, **Settings > Check for updates** installs new CrossInk-Tailnet
releases over the air. To go back to stock CrossInk-Bookorbit, flash its `.bin` the same way.

## Joining your tailnet

Tailscale ships **off**. Nothing tailnet-related runs until you turn it on.

1. Open **Settings > Tailscale** and choose **Log in**. It turns Tailscale on and, if WiFi is
   off, asks for a network first.
2. Scan the QR code with a phone that is signed in to Tailscale and approve the device. The
   screen then shows the reader's `100.x.y.z` address. From then on the reader joins the tailnet
   whenever WiFi is on.
3. In the [admin console](https://login.tailscale.com/admin/machines), tag the reader as described
   in [Recommended tailnet setup](#recommended-tailnet-setup) (recommended; required only on
   tailnets of several hundred devices). If you don't tag it, **disable key expiry** for it instead: a device that joined
   by QR cannot renew its own key. Optionally rename it (default `esp32-xxyyzz`).
4. Point BookOrbit (or OPDS) at the server's tailnet name, its `100.x` address, or a public DNS
   name that points at the `100.x` address (e.g. `https://books.example.net`).

**Log out** in the same screen erases the reader's tailnet keys; remove the old device in the
admin console too. Turning Tailscale off keeps the login for later.

## Recommended tailnet setup

Recommended for every tailnet: give the reader its own tag and let it reach only your BookOrbit
server. Tailscale sends each device only the peers it may talk to (or that may talk to it), so the
reader then receives a list of about one device instead of your whole tailnet.

- **The reader stores its tailnet key unencrypted.** Tagged and restricted, a lost reader can
  reach one service, not your tailnet.
- **Tagged devices don't expire.** You don't need to disable key expiry by hand.
- **MicroLink is small.** It tracks 16 peers (8 on the X3); on a larger tailnet the overlay pins
  your server into that table (see below), at the cost of a restart. It also reads at most 512 KB of
  the device list (roughly 350 devices); past that, only a smaller list helps.

In the admin console, **Access controls**, add (adapt the address and port):

```jsonc
"tagOwners": {
  "tag:ereader": ["autogroup:admin"],
},
"grants": [
  // E-readers reach only the BookOrbit server (add "tcp:80" for plain HTTP).
  {"src": ["tag:ereader"], "dst": ["100.69.222.123"], "ip": ["tcp:443"]},
],
```

Then in **Machines**, open the reader (`esp32-…`) → **Edit ACL tags** → add `tag:ereader`. It keeps
its login.

**Don't give the reader Mullvad.** With Tailscale's Mullvad add-on, every device that has Mullvad
access receives all of Mullvad's ~530 exit nodes in its device list, far more than MicroLink can
read. A `nodeAttrs` entry granting `"mullvad"` to a user group (e.g. `autogroup:member`) does not
apply to tagged devices, so tagging the reader takes care of it; just don't add `tag:ereader` to
such an entry.

**Check for broad rules.** A rule whose `dst` is `"*"` (or a group the reader falls into) lets its
sources reach the reader, and those sources are then sent to the reader too. Name the destinations
of such rules explicitly instead. On the reader, **Settings > Tailscale > Diagnostics** should show
only one or two peers tracked once this is right.

### Large tailnets

MicroLink keeps a small peer table (16 on the X4 Pro/Classic, 8 on the X3). When the reader
connects to a tailnet address that isn't in it, the overlay pins that address as MicroLink's
priority peer, remembers it across reboots, and restarts MicroLink (a few seconds) so the server
is added; the request waits for it. It tries once per address per WiFi session, and the most
recent such address is the one pinned. Pinning cannot recover a server whose entry falls beyond
the 512 KB that MicroLink reads: Tailscale's protocol has no way to ask for a single device.

### Relay regions

When a direct connection isn't possible (guest WiFi, carrier NAT, Kubernetes), traffic goes through
Tailscale's DERP relays, which are grouped into regions that don't forward to each other. Stock
MicroLink uses one fixed region (Dallas) for everything. The overlay's MicroLink patch makes it
behave like a normal Tailscale client:

- **Closest region.** Each time it connects, the reader times a STUN probe to every relay region
  and makes the fastest its home (kept unless another is a third faster). With UDP blocked it
  keeps its previous home.
- **Per-peer relays.** Packets for a peer go through that peer's home region. The reader opens a
  second relay connection for it on demand (one at a time, ~25 KB of RAM) and closes it after a
  minute idle; if that region can't be reached it falls back to its own.

**Settings > Tailscale > Relay region** overrides the choice of home region:

- **Automatic (closest)**: the default, as above.
- **Match BookOrbit server**: the reader's home follows the home region of the tailnet server it
  connects to (learned on first use and remembered; it restarts MicroLink once if the server moves).
  Useful when UDP is blocked, so the probe can't run and nothing needs the second connection.
- **A fixed region** (e.g. `sea`): listed once the reader has connected at least once.

**Settings > Tailscale > Diagnostics** shows both regions ("Relay: ours sea, server sea").

## Limits

- Only tailnet hosts go through the tunnel. Internet traffic (GitHub updates, public OPDS
  catalogs) goes direct: MicroLink cannot use an exit node.
- Tailnet names come from the reader's peer list, not MagicDNS. Short names and full
  `*.ts.net` names of peers work; other MagicDNS records do not. Public DNS records pointing at
  `100.x` addresses work, unless your router's DNS rebinding protection strips such answers.
- The tailnet key is stored in the reader's internal flash, unencrypted (MicroLink's design).
  Limit the reader with an ACL tag, and remove it in the admin console if it's lost.
- MicroLink is an independent, beta implementation of the Tailscale protocol (see its README).
- One relay connection besides the reader's own: servers homed in several different relay regions
  take turns (each switch costs a TLS handshake).

## Building locally

```bash
git clone --recursive https://github.com/agosez/CrossInk-Bookorbit
git clone --recursive https://github.com/Fledgewing/CrossInk-Tailnet
CrossInk-Tailnet/scripts/apply.sh CrossInk-Bookorbit
cd CrossInk-Bookorbit && pio run -e x4-pro    # or x4-classic, default
```

`apply.sh` only touches git-ignored paths plus the settings patch in CrossInk, applies the MicroLink
patch to this repo's submodule, and is safe to rerun.

## Licence

MIT, like CrossInk and MicroLink (see `third_party/microlink/LICENSE` for MicroLink's notices,
including WireGuard and X25519). Not affiliated with or endorsed by Tailscale Inc.
