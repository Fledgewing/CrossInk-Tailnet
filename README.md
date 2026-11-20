# CrossInk-Tailnet

[CrossInk-Bookorbit](https://github.com/agosez/CrossInk-Bookorbit) firmware with your reader on
your Tailscale tailnet, using [MicroLink](https://github.com/CamM2325/microlink). BookOrbit sync,
the catalog, OPDS and downloads can reach servers that are only on your tailnet.

This is an **overlay**, not a fork. CrossInk's source is used as released; the overlay adds:

| Piece | Where | What it does |
|---|---|---|
| MicroLink | `third_party/microlink` (submodule, pinned tag) | Tailscale-compatible WireGuard client for ESP32 |
| Shim | `tailnet/shim` | Starts MicroLink when WiFi gets an address, stops it when WiFi goes away; answers tailnet names (`bookorbit`, `bookorbit.<tailnet>.ts.net`) for every HTTP client in the firmware |
| Login screen | `tailnet/ui` | Shows the Tailscale login as a QR code the first time it's needed |
| Build hook | `tailnet/tailnet_build.py` + `overlay/platformio.local.ini` | Compiles the above into CrossInk's normal PlatformIO build and points the updater at this repo's releases |
| UI hook | `overlay/patches/0001-tailnet-ui-hook.patch` | Two lines in `src/main.cpp`: the only edit to CrossInk's source |

When CrossInk-Bookorbit releases, the daily CI run rebuilds the new tag with the overlay and
publishes a release here. The only thing that can need hand work is the two-line patch, and CI
says so if it stops applying.

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

1. Connect the reader to WiFi (any network screen does it: BookOrbit sync, catalog, file transfer).
2. The **Tailscale** screen shows a QR code. Scan it with a phone that is signed in to Tailscale
   and approve the device. The screen then shows the reader's `100.x.y.z` address.
3. In the [admin console](https://login.tailscale.com/admin/machines):
   - **Disable key expiry** for the reader. A device that joined by QR cannot renew its own key.
   - Optionally rename it (default `esp32-xxyyzz`) and tag it with an ACL that only lets it reach
     your BookOrbit server. The reader keeps its tailnet key unencrypted in flash.
4. Point BookOrbit (or OPDS) at the server's tailnet name or `100.x` address, e.g.
   `http://bookorbit:8080`.

## Limits

- Only tailnet hosts go through the tunnel. Internet traffic (GitHub updates, public OPDS
  catalogs) goes direct: MicroLink cannot use an exit node.
- Tailnet names come from the reader's peer list, not MagicDNS. Short names and full
  `*.ts.net` names of peers work; other MagicDNS records do not.
- MicroLink is an independent, beta implementation of the Tailscale protocol (see its README).
- The DERP home region defaults to Dallas (9). Set `CONFIG_ML_DERP_REGION` in
  `tailnet/ml_config.h` if you're far from it.

## Building locally

```bash
git clone --recursive https://github.com/agosez/CrossInk-Bookorbit
git clone --recursive https://github.com/Fledgewing/CrossInk-Tailnet
CrossInk-Tailnet/scripts/apply.sh CrossInk-Bookorbit
cd CrossInk-Bookorbit && pio run -e x4-pro    # or x4-classic, default
```

`apply.sh` only touches git-ignored paths plus the two-line patch, and is safe to rerun.

## Licence

MIT, like CrossInk and MicroLink (see `third_party/microlink/LICENSE` for MicroLink's notices,
including WireGuard and X25519). Not affiliated with or endorsed by Tailscale Inc.
