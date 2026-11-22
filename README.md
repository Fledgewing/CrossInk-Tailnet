# CrossInk-Tailnet

[CrossInk-Bookorbit](https://github.com/agosez/CrossInk-Bookorbit) firmware with your reader on
your Tailscale tailnet, using [MicroLink](https://github.com/CamM2325/microlink). BookOrbit sync,
the catalog, OPDS and downloads can reach servers that are only on your tailnet.

This is an **overlay**, not a fork. CrossInk's source is used as released; the overlay adds:

| Piece | Where | What it does |
|---|---|---|
| MicroLink | `third_party/microlink` (submodule: the `crossink` branch of [our fork](https://github.com/Fledgewing/microlink)) | Tailscale-compatible WireGuard client for ESP32, with our changes (closest relay region, per-peer relays) on top |
| Shim | `tailnet/shim` | Starts MicroLink when WiFi gets an address, stops it when WiFi goes away; answers tailnet names (`bookorbit`, `bookorbit.<tailnet>.ts.net`) for every HTTP client in the firmware |
| Settings screen | `tailnet/ui` | **Settings > Tailscale**: on/off, status and address, log in (QR code) and log out |
| Build hook | `tailnet/tailnet_build.py` + `overlay/platformio.local.ini` | Compiles the above into CrossInk's normal PlatformIO build and points the updater at this repo's releases |
| CrossInk patches | `overlay/patches/v1.6/` (CrossInk 1.6.x), `overlay/patches/v2.0/` (CrossInk 2.x) | One numbered series per upstream line: the Settings row (0001), the framebuffer freed while Tailscale runs in a network boot on the X3/X4 (0002, `tailnet/shim/TailnetNetPhase.h`), and the memory and download changes after them |

When CrossInk-Bookorbit releases, the daily CI run rebuilds the new tag with the overlay and
publishes a release here. The only thing that can need hand work is the patch series for that upstream
line, and CI says so if it stops applying. The shim, MicroLink and the switch table are shared by both lines.

## Readers

| Reader | Env | Notes |
|---|---|---|
| X4 Pro | `x4-pro` | ESP32-S3 + PSRAM. Primary target. |
| X4 Classic | `x4-classic` | ESP32-S3 + PSRAM. |
| X3 / X4 | `default` | ESP32-C3, no PSRAM. Built with English UI only to make room; smaller MicroLink buffers. |

The partition table is never changed (that can only be done over USB).

### X3 (rc2 / v2.0) differences

CrossInk 2.x plus Tailscale does not fit the ESP32-C3's app slot, so the `default` build
(X3 and the C3 X4) of CrossInk 2.x leaves out four upstream features. The X4 Pro and X4 Classic
keep everything. Each is a switch in `tailnet/tailnet_build.py` (`CROSSINK_TAILNET_NO_*`).

| Removed on the X3 | Workaround |
|---|---|
| Nearby transfer (send a book, nearby position and stats sync) | Use File Transfer (web) or the SD card to move books. |
| mDNS (`<name>.local`) | Connect Calibre and the web file transfer with the IP address shown on the reader's screen. |
| Plugins (hub, catalog, plugin events, the web plugin pages) | None on the X3. |
| Language and hyphenation pack downloads | Install from the SD card (below). Packs already on the card keep working. |

To install from the SD card, download `crossink-languages-<version>.zip` from the
CrossInk-Bookorbit release that matches the firmware (upstream CrossInk's packs lack this fork's
screens). Copy its `.crossink` folder to the root of the SD card. It holds both
`.crossink/languages` and `.crossink/hyphenation` (`hyph-<code>.cphyph`). Then:

- **Languages:** pick one in **Settings > System > Device > Language**.
- **Hyphenation:** open **Settings > Reader > Page Layout > Hyphenation Packs** and select a
  pack marked **Install**. Installed packs offer **Delete** only. To update a pack, delete it,
  copy the new file and install it again.

Where the downloads used to be, the reader shows where to copy the files.

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
- **Per-peer relays** (off by default). Packets for a peer can go through that peer's home region
  over a second relay connection, opened on demand and closed after a minute idle. Each costs a TLS
  session of internal RAM, which the X4 Pro can't spare next to CrossInk's own connections, so it's
  built with `CONFIG_ML_DERP_EXTRA_CONNS 0` (`tailnet/ml_config.h`). Peers running Tailscale reach
  the reader through its home region anyway.

**Settings > Tailscale > Relay region** overrides the choice of home region:

- **Match BookOrbit server** (default): the reader's home follows the home region of the tailnet server it
  connects to (learned on first use and remembered; it restarts MicroLink once if the server moves).
  Until it has learned the server's region it uses the closest.
- **Automatic (closest)**: as above.
- **A fixed region** (e.g. `sea`): listed once the reader has connected at least once.

**Settings > Tailscale > Diagnostics** shows both regions ("Relay: ours sea, server sea").

## Sync while reading (X4 Pro, X4 Classic)

With BookOrbit **Smart sync** on, **Settings > BookOrbit Sync > Sync While Reading** syncs the
open book in the background every 10, 15, 30 or 60 minutes, every 25, 50 or 100 pages, or every
one or two chapters (off by default). The book stays open: WiFi and Tailscale come up, the
progress is synced with Smart sync's rules, and WiFi goes off again. A newer position on the
server is never overwritten; it waits for the next sync when you open the book or the reader
sleeps, as does any case Smart sync would ask about. Highlights, bookmarks and reading stats
also wait for that sync. Nothing is shown while reading; without a network it simply tries
again at the next interval. Not on the X3: a sync does not fit in its memory beside an open
book.

## Settings PIN

**Settings > System > Settings PIN** sets an optional 4-digit PIN (Up/Down change a digit,
Confirm moves on). While one is set, the reader asks for it before opening Settings, the
BookOrbit, KOReader and OPDS account screens, Quick Actions and button setup, the plugin
installer, and before joining or forgetting a WiFi network. The web file transfer answers
"settings locked" for its settings, WiFi, OPDS, font and plugin-file pages, and hidden files
(`.crosspoint`, `.crossink`) stay hidden on the reader, in the web file manager and over
WebDAV. Reading, the library, the BookOrbit catalog, downloads and sync stay open. Once
entered, the PIN holds until you leave Settings or the reader sleeps.

It is a lock against children, not security: anyone can take the SD card out and edit the
settings files. The PIN is kept in the reader's internal memory, so deleting the settings file
does not remove it. If it is forgotten, create an empty file named `unlock-settings` in the
`.crossink` folder of the SD card and restart the reader: the PIN is removed and the file
deleted.

## Battery stats

The reader keeps a small log of how its battery is used: one 16-byte record whenever it wakes,
sleeps, restarts, turns WiFi on or off, is plugged in or unplugged, opens or closes a book,
turns the frontlight on or off, or its battery level changes while awake. The log is
`.crossink/battery.bin` on the SD card, about 1.6 KB a day. Nothing wakes the reader to
record it, and nothing leaves the reader on its own.

**Settings > System > Battery stats** writes `battery_stats.json` to the root of the SD card in
the `orbit-battery-stats/1` format, which Orbit Companion's Battery › Compare reads. On the X4
Pro and X4 Classic the same page shows the time left and the battery used per hour asleep,
per hour reading and per 100 pages, and the file is written again whenever USB Drive mode
starts. The X3 and X4 build (`default`) writes the file without the numbers on the page.

## Limits

- Only tailnet hosts go through the tunnel. Internet traffic (GitHub updates, public OPDS
  catalogs) goes direct: MicroLink cannot use an exit node.
- Tailnet names come from the reader's peer list, not MagicDNS. Short names and full
  `*.ts.net` names of peers work; other MagicDNS records do not. Public DNS records pointing at
  `100.x` addresses work, unless your router's DNS rebinding protection strips such answers.
- The tailnet key is stored in the reader's internal flash, unencrypted (MicroLink's design).
  Limit the reader with an ACL tag, and remove it in the admin console if it's lost.
- MicroLink is an independent, beta implementation of the Tailscale protocol (see its README).

## Building locally

```bash
git clone --recursive https://github.com/agosez/CrossInk-Bookorbit
git clone --recursive https://github.com/Fledgewing/CrossInk-Tailnet
CrossInk-Tailnet/scripts/apply.sh CrossInk-Bookorbit
cd CrossInk-Bookorbit && pio run -e x4-pro    # or x4-classic, default
```

`apply.sh` picks the patch series from the checkout's CrossInk version (`[crossink] version` in its
`platformio.ini`, else its tag): 1.6.x uses `overlay/patches/v1.6`, 2.x uses `overlay/patches/v2.0`, and
any other version stops with an error. `apply.sh --series v1.6|v2.0 <checkout>` (or
`CROSSINK_PATCH_SERIES=v2.0`) chooses one explicitly. Besides that series it only touches git-ignored
paths in CrossInk, and it is safe to rerun.

CrossInk 1.6 and 2.x build on different pioarduino platforms that share one framework package
directory, so keep a separate PlatformIO home for each, e.g.
`PLATFORMIO_CORE_DIR=~/.platformio-rc2 pio run -e default` for 2.x.

To rebuild a published release, use the commits from its notes: clone CrossInk-Bookorbit at the
release's upstream tag (`git clone --recursive --branch <tag> ...`), check out this repository at
the release's overlay commit (`git checkout <commit> && git submodule update --init --recursive`),
then run the same commands. CI builds with PlatformIO Core 6.1.19 (pioarduino); see
`.github/workflows/build.yml` at that commit.

## License

- **Source in this repository** (the overlay): MIT, see [`LICENSE`](LICENSE). MicroLink, the
  submodule, is MIT too (`third_party/microlink/LICENSE`, with its WireGuard and X25519 notices).
- **Released firmware binaries** (`firmware-*.bin`): they combine CrossInk-Bookorbit, MicroLink,
  this overlay, the Arduino core, ESP-IDF and other third-party components, including
  [wolfSSL](https://github.com/wolfSSL/Arduino-wolfSSL), which is licensed GPL-2.0-or-later. The
  binaries are therefore distributed under the **GNU General Public License v3.0 or later**
  ([`licenses/GPL-3.0.txt`](licenses/GPL-3.0.txt)). Each component keeps its own license for its
  own code.
- **Third-party list:** [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md) lists every component
  in the firmware with its version, license, copyright and source; [`licenses/`](licenses/) holds
  the license texts and notices. Every release attaches both (`licenses.zip`).
- **Corresponding source:** each release's notes ("Licenses and source") link the exact overlay
  commit, the CrossInk-Bookorbit tag, the MicroLink commit and these build instructions
  ([Building locally](#building-locally)). The upstream tag's `platformio.ini` pins the platform and
  every library version. Releases also attach `source-<version>.tar.gz` with all of that source,
  including the libraries and the Arduino core, in one archive.

Not affiliated with or endorsed by Tailscale Inc.
