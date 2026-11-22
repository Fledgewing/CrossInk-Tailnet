# Third-party licenses

This file lists every component compiled or linked into the CrossInk-Tailnet firmware
binaries (`firmware-*.bin` on the [releases](https://github.com/Fledgewing/CrossInk-Tailnet/releases)
page), with its version, license, copyright and source. The full license texts and
each component's own notice files are in [`licenses/`](licenses/); every release also
attaches this file and `licenses.zip`.

The list was taken from the linker map files of real builds of both lines (every
library and archive the linker pulled in, for all three reader builds), not from
dependency declarations alone.

This is a good-faith summary, not legal advice. Where a component's own files say
something different, they win.

## How the firmware binaries are licensed

- The **source in this repository** (the overlay: `tailnet/`, `overlay/`, `scripts/`) is
  MIT-licensed ([`LICENSE`](LICENSE)).
- The **firmware binaries** statically link [wolfSSL](https://github.com/wolfSSL/Arduino-wolfSSL)
  (pulled in by CrossInk-Bookorbit for TLS), which is licensed **GPL-2.0-or-later**. They also link
  Apache-2.0 code (ESP-IDF, the Espressif Wi-Fi libraries, mbedTLS, PNGdec, JPEGDEC, AnimatedGIF), which is
  compatible with GPL-3.0 but not with GPL-2.0-only. Using wolfSSL's "or any later version"
  option, the combined binaries are distributed under the
  **GNU General Public License v3.0 or later** ([`licenses/GPL-3.0.txt`](licenses/GPL-3.0.txt)).
  Every other component keeps its own license for its own code; all of them are
  GPL-3.0-compatible as used here (see [Known issues](#known-issues) for the exceptions to check).
- **Corresponding source** for each binary release is public; the release notes link
  the exact commits (see [Corresponding source](#corresponding-source)).

## Release lines

| Line | Releases | Upstream | Platform | Arduino core | ESP-IDF (prebuilt) | Toolchain |
|---|---|---|---|---|---|---|
| v1.6 | `v1.6.1.*+bookorbit.8` | [CrossInk-Bookorbit `v1.6.1+bookorbit.8`](https://github.com/agosez/CrossInk-Bookorbit/tree/v1.6.1%2Bbookorbit.8) | [pioarduino platform-espressif32 55.03.37](https://github.com/pioarduino/platform-espressif32/releases/tag/55.03.37) | arduino-esp32 3.3.7 | 5.5 (`release/v5.5` @ `87912cd291`, lib-builder `8cabf2c`) | GCC 14.2.0 (esp-14.2.0_20251107) |
| v2.0 | `v2.0.0.*+bookorbit.1-rc2` | [CrossInk-Bookorbit `v2.0.0+bookorbit.1-rc2`](https://github.com/agosez/CrossInk-Bookorbit/tree/v2.0.0%2Bbookorbit.1-rc2) | [pioarduino platform-espressif32 55.03.311](https://github.com/pioarduino/platform-espressif32/releases/tag/55.03.311) | arduino-esp32 3.3.11 | 5.5.5 (`v5.5.5` @ `b774170ff46`, lib-builder `ee57070`) | GCC 14.2.0 (esp-14.2.0_20260121) |

Both lines are built with PlatformIO Core 6.1.19 (pioarduino), as in
`.github/workflows/build.yml`.

## Components

"Both" means the component is in both lines at the version shown. `licenses/notices/`
holds each component's own license or notice file, copied unmodified.

### Firmware applications and overlay

| Component | Version | License | Copyright | Source | Notice file |
|---|---|---|---|---|---|
| CrossInk-Tailnet overlay (shim, settings UI, build hook, patches) | release's overlay commit | MIT | Copyright (c) 2026 Fledgewing | [Fledgewing/CrossInk-Tailnet](https://github.com/Fledgewing/CrossInk-Tailnet) | `CrossInk-Tailnet.txt` |
| CrossInk-Bookorbit (via CrossInk, CrossPoint Reader) | see Release lines | MIT | v1.6: Copyright (c) 2025 Dave Allie; v2.0: also Copyright (c) 2025 CrossPoint Reader organization | [agosez/CrossInk-Bookorbit](https://github.com/agosez/CrossInk-Bookorbit) | `crossink-bookorbit-v1.6.txt`, `crossink-bookorbit-v2.0.txt` |
| FreeInk SDK (submodule `freeink-sdk`) | v1.6: `6993701`; v2.0: `aad0749` | MIT (includes MIT code from the OpenX4 E-Paper Community SDK) | Copyright (c) 2026 FreeInk; Copyright (c) 2025 Open X4 E-Paper Contributors | [Free-Ink/freeink-sdk](https://github.com/Free-Ink/freeink-sdk) | `freeink-sdk-v1.6.txt`, `freeink-sdk-v2.0.txt` |
| BMI270 configuration file (in FreeInk SDK `Imu`) | v2.0 only | BSD-3-Clause | Copyright (c) 2023 Bosch Sensortec GmbH | [BMI270_SensorAPI](https://github.com/boschsensortec/BMI270_SensorAPI) | `bmi270-config.txt` |
| MicroLink (submodule `third_party/microlink`) | release's submodule commit (see release notes) | MIT | Copyright (c) 2025-2026 Cameron Malone | [Fledgewing/microlink](https://github.com/Fledgewing/microlink) (`crossink` branch), fork of [CamM2325/microlink](https://github.com/CamM2325/microlink); v1.6.1.3 to v1.6.1.21 used CamM2325/microlink plus the overlay's `overlay/patches-microlink` | `MicroLink.txt` |
| wireguard-lwip (in MicroLink) | as in MicroLink | BSD-3-Clause | Copyright (c) 2021 Daniel Hope (www.floorsense.nz) | [smartalock/wireguard-lwip](https://github.com/smartalock/wireguard-lwip) | `wireguard-lwip.txt` |
| X25519 from STROBE (in MicroLink) | as in MicroLink | MIT | Copyright (c) 2015-2016 Cryptography Research, Inc. | [STROBE](https://sourceforge.net/p/strobe) | `MicroLink-x25519.txt` |
| poly1305-donna (in MicroLink) | as in MicroLink | Public domain or MIT | Andrew Moon | [floodyberry/poly1305-donna](https://github.com/floodyberry/poly1305-donna) | `misc.txt` |
| TweetNaCl-derived crypto_box (in MicroLink) | as in MicroLink | Public domain | D. J. Bernstein et al. | [tweetnacl.cr.yp.to](https://tweetnacl.cr.yp.to/) | `misc.txt` |

### Libraries (PlatformIO `lib_deps`)

| Component | Version | License | Copyright | Source | Notice file |
|---|---|---|---|---|---|
| **Arduino-wolfSSL** | 5.7.2 (both) | **GPL-2.0-or-later** (or commercial) | Copyright (C) 2006-2023 wolfSSL Inc. | [wolfSSL/Arduino-wolfSSL](https://github.com/wolfSSL/Arduino-wolfSSL) | `wolfSSL.txt`, `../GPL-2.0.txt` |
| WebSockets | 2.7.3 (both) | LGPL-2.1-or-later | Copyright (c) 2015 Markus Sattler. All rights reserved. | [Links2004/arduinoWebSockets](https://github.com/Links2004/arduinoWebSockets) | `WebSockets.txt` |
| SdFat (patched by upstream's `scripts/sdfat_patches`) | 2.3.1 (both) | MIT | Copyright (c) 2011..2020 Bill Greiman | [greiman/SdFat](https://github.com/greiman/SdFat) | `SdFat.txt` |
| ArduinoJson | 7.4.2 (both) | MIT | Copyright (c) 2014-2025, Benoit Blanchon | [bblanchon/ArduinoJson](https://github.com/bblanchon/ArduinoJson) | `ArduinoJson.txt` |
| QRCode | 0.0.1 (both) | MIT | Copyright (c) 2017 Richard Moore | [ricmoo/QRCode](https://github.com/ricmoo/QRCode) | `QRCode.txt` |
| PNGdec | 1.1.6 (both) | Apache-2.0 | Copyright 2020 BitBank Software, Inc. | [bitbank2/PNGdec](https://github.com/bitbank2/PNGdec) | `PNGdec.txt` |
| JPEGDEC | git `86282979` (both) | Apache-2.0 | Copyright 2020 BitBank Software, Inc. | [bitbank2/JPEGDEC](https://github.com/bitbank2/JPEGDEC/tree/86282979224c8a32fd51e091ed5a35b0c699a52b) | `JPEGDEC.txt` |
| AnimatedGIF | git `67cfacaf` (v2.0 only) | Apache-2.0 | Copyright 2020 BitBank Software, Inc. | [bitbank2/AnimatedGIF](https://github.com/bitbank2/AnimatedGIF/tree/67cfacafe12ef4926c7713342c57d2a4af040e5e) | `AnimatedGIF.txt` |

None of the Apache-2.0 components above ships a `NOTICE` file.

### Code vendored inside CrossInk-Bookorbit and the FreeInk SDK

| Component | Version | License | Copyright | Where | Notice file |
|---|---|---|---|---|---|
| Expat | 2.7.3 (both) | MIT | Copyright (c) 1998-2000 Thai Open Source Software Center Ltd and Clark Cooper; Copyright (c) 2001-2025 Expat maintainers | `lib/expat` | `expat.txt` |
| miniz | 11.3.2 (both) | MIT | Copyright 2013-2014 RAD Game Tools and Valve Software; Copyright 2010-2014 Rich Geldreich and Tenacious Software LLC | `lib/miniz`; v2.0 also FreeInk SDK `ContentProtection` | `miniz.txt` |
| uzlib | 2.9.8 (both) | Zlib | Copyright (c) 2003 Joergen Ibsen / Jibz; Copyright (c) 2014-2018 Paul Sokolovsky | `lib/uzlib` | `uzlib.txt` |
| MiniBidi | both | MIT (original); mintty changes | Ahmad Khalifa; changes by Thomas Wolff | `lib/MiniBidi` | `misc.txt` |
| FreeType (X4 Pro and X4 Classic builds only) | 2.13.3 (both) | FreeType License (FTL), chosen over GPL-2.0 | Portions of this software are copyright (c) 1996-2024 The FreeType Project (www.freetype.org). All rights reserved. | FreeInk SDK `FreeInkFont/third_party/freetype` | `FreeType.txt`, `../FTL.txt` |
| stb_truetype (X4 builds) | both | MIT or public domain | Copyright (c) 2017 Sean Barrett | FreeInk SDK `FreeInkFont/third_party/stb` | `stb_truetype.txt` |
| Tabler Icons (converted to bitmaps) | both | MIT | Copyright (c) 2020-2026 Paweł Kuna | `src/components/icons` from [tabler/tabler-icons](https://github.com/tabler/tabler-icons) | `tabler-icons.txt` |

### Fonts (converted to bitmap or embedded font data)

| Font | License | Copyright | Notice file |
|---|---|---|---|
| Bitter | OFL-1.1 | Copyright 2011 The Bitter Project Authors (https://github.com/solmatas/BitterPro), Reserved Font Name "Bitter Pro" | `font-Bitter.txt` |
| Lexend Deca | OFL-1.1 | Copyright 2018 The Lexend Project Authors (https://github.com/googlefonts/lexend), Reserved Font Name "RevReading Lexend" | `font-LexendDeca.txt` |
| Inter | OFL-1.1 | Copyright (c) 2016 The Inter Project Authors (https://github.com/rsms/inter) | `font-Inter.txt` |
| Noto Sans Symbols 2 (UI symbols) | OFL-1.1 | Copyright 2022 The Noto Project Authors (https://github.com/notofonts/symbols) | `font-NotoSansSymbols2.txt` |

Fonts in upstream's source tree that are not compiled into the firmware (Charis SIL /
ChareInk7, Noto Sans, Noto Sans Arabic, Noto Sans CJK SC, Noto Emoji) are not listed.
Full text: [`licenses/OFL-1.1.txt`](licenses/OFL-1.1.txt).

### Hyphenation patterns (compiled into hyphenation tables)

From [Typst Hypher](https://github.com/typst/hypher) `d81cc50` (hyph-utf8 patterns). The
unmodified header of each pattern file, with its copyright and licence, is in
`licenses/notices/hyphenation-patterns.txt`.

| Language | In firmware | License | Copyright |
|---|---|---|---|
| English (US) | both | Permissive (copying and distribution permitted with notice) | Copyright (C) 1990, 2004, 2005 Gerard D.C. Kuiken |
| German (1996) | v1.6 | MIT | Copyright (c) 2013-2024 Stephan Hennig, Werner Lemberg, Günter Milde, Sander van Geloven, Georg Pfeiffer, Gisbert W. Selke, Tobias Wendorf, Keno Wehr |
| Spanish | v1.6 | MIT | Copyright (C) 1993, 1997, 2001-2019 Javier Bezos, CervanTeX |
| French | v1.6 | MIT | Copyright (C) 1994-2002 Daniel Flipo, Bernard Gaulle, 2016 Arthur Reutenauer |
| Italian | v1.6 | LPPL or MIT (MIT chosen) | Copyright (C) 2008-2011 Claudio Beccari |
| Polish | v1.6 | MIT (among others) | Copyright (C) 1987-1995 Hanna Kołodziejska, Bogusław Jackowski, Marek Ryćko |
| Portuguese | v1.6 | BSD-3-Clause | Copyright (C) 1987, 1994, 1996, 2015 Pedro J. de Rezende, 1996, 2015 J. Joao Dias Almeida, 2024 Leonardo Araujo and Aline Benevides |
| Russian | v1.6 | LPPL | Copyright (C) 1999-2003 Alexander I. Lebedev |
| Swedish | v1.6 | LPPL | Copyright (C) 1994 Jan Michael Rynning |
| Ukrainian | v1.6 | LPPL | Copyright (C) 1998-2001 Maksym Polyakov |

In v2.0 only English is built in; the other languages are separate downloadable packs
from CrossInk-Bookorbit, not part of these binaries.

### Platform: Arduino core, ESP-IDF and toolchain runtime

| Component | License | Copyright | Source | Notice file |
|---|---|---|---|---|
| arduino-esp32 core and bundled libraries (WiFi, Network, HTTPClient, WebServer, NetworkClientSecure, FS, Preferences, ESPmDNS, AsyncUDP, DNSServer, Ethernet, SPI, Wire, USB) | LGPL-2.1-or-later (some files Apache-2.0 or MIT, as marked in them) | Copyright Espressif Systems and contributors | [espressif/arduino-esp32](https://github.com/espressif/arduino-esp32) | `arduino-esp32.txt` |
| ESP-IDF (prebuilt in `framework-arduinoespressif32-libs`) | Apache-2.0 | Copyright Espressif Systems (Shanghai) Co., Ltd. | [espressif/esp-idf](https://github.com/espressif/esp-idf), [esp32-arduino-lib-builder](https://github.com/espressif/esp32-arduino-lib-builder) | `../Apache-2.0.txt` |
| Espressif Wi-Fi, PHY, coexistence, ESP-NOW and mesh libraries (binary only) | Apache-2.0 | Copyright Espressif Systems (Shanghai) Co., Ltd. | [esp32-wifi-lib](https://github.com/espressif/esp32-wifi-lib), [esp-phy-lib](https://github.com/espressif/esp-phy-lib) | `misc.txt` |
| ESP-IDF managed components: mdns, esp_diagnostics, network_provisioning | Apache-2.0 | Copyright Espressif Systems | [espressif/esp-protocols](https://github.com/espressif/esp-protocols), [esp-insights](https://github.com/espressif/esp-insights), [network_provisioning](https://github.com/espressif/network_provisioning) | `../Apache-2.0.txt` |
| lwIP | BSD-3-Clause | Copyright (c) 2001, 2002 Swedish Institute of Computer Science | [lwip-tcpip/lwip](https://github.com/lwip-tcpip/lwip) | `lwIP.txt` |
| Mbed TLS | Apache-2.0 (chosen) or GPL-2.0-or-later | Copyright The Mbed TLS Contributors | [Mbed-TLS/mbedtls](https://github.com/Mbed-TLS/mbedtls) | `mbedTLS.txt` |
| FreeRTOS kernel | MIT | Copyright (C) Amazon.com, Inc. or its affiliates | [FreeRTOS/FreeRTOS-Kernel](https://github.com/FreeRTOS/FreeRTOS-Kernel) | `FreeRTOS.txt` |
| wpa_supplicant / hostapd | BSD-3-Clause | Copyright (c) 2002-2022, Jouni Malinen and contributors | [w1.fi](https://w1.fi/) | `wpa_supplicant.txt` |
| protobuf-c | BSD-2-Clause | Copyright (c) 2008-2022, Dave Benson and the protobuf-c authors | [protobuf-c/protobuf-c](https://github.com/protobuf-c/protobuf-c) | `protobuf-c.txt` |
| http_parser | MIT | Copyright Joyent, Inc. and other Node contributors; based on NGINX, copyright Igor Sysoev | [nodejs/http-parser](https://github.com/nodejs/http-parser) | `http_parser.txt` |
| cJSON (IDF `json`; v2.0 X4 Pro also `espressif__cjson`) | MIT | Copyright (c) 2009-2017 Dave Gamble and cJSON contributors | [DaveGamble/cJSON](https://github.com/DaveGamble/cJSON) | `cJSON.txt` |
| TLSF allocator | BSD-3-Clause | Copyright (c) 2006-2016 Matthew Conte | ESP-IDF `components/heap/tlsf` | `misc.txt` |
| TinyUSB (X4 builds) | MIT | Copyright (c) 2019 Ha Thach (tinyusb.org) | [hathach/tinyusb](https://github.com/hathach/tinyusb) | `TinyUSB.txt` |
| newlib C library | BSD-style and similar permissive licenses, per file | Various, see notice | [sourceware.org/newlib](https://sourceware.org/newlib/) | `newlib.txt` |
| GCC runtime (libgcc, libstdc++) | GPL-3.0 with the GCC Runtime Library Exception 3.1 | Copyright Free Software Foundation, Inc. | [gcc.gnu.org](https://gcc.gnu.org/) | `../GCC-exception-3.1.txt` |

The prebuilt framework package also carries components that the linker does not pull into
these binaries (for example libhelix-mp3, libsodium, littlefs, ESP RainMaker, Zigbee, esp-sr);
they are not part of the firmware and are not listed.

## Corresponding source

For each release, the release notes ("Licenses and source") link:

1. the exact overlay commit of this repository (its `.github/workflows/build.yml` is the
   build recipe, `scripts/apply.sh` applies the overlay);
2. the CrossInk-Bookorbit tag, whose `platformio.ini` pins the platform and every library
   version or commit, and whose `freeink-sdk` submodule commit is fixed by the tag;
3. the MicroLink commit (the `third_party/microlink` submodule at that overlay commit).

Releases from v1.6.1.33 and v2.0.0.32 on also attach `source-<version>.tar.gz`: all of the
above in one archive (the overlay with MicroLink, CrossInk after the overlay with the FreeInk
SDK, each reader's PlatformIO library sources as fetched, the Arduino core source and a
`BUILD.md`). It leaves out only the public pioarduino platform package (prebuilt ESP-IDF and
toolchain), which `BUILD.md` links. For older releases the archive is available on request.

To rebuild a release: check out the overlay commit with submodules and the upstream tag
with submodules, then follow [Building locally](README.md#building-locally). PlatformIO
fetches the platform, the Arduino core, the prebuilt ESP-IDF libraries and the libraries at
the pinned versions.

## Known issues

These are recorded rather than papered over. None is known to have been raised by a
rights holder.

1. **Espressif binary libraries and GPL source.** GPL-3.0 asks for the complete source of
   the whole binary. Espressif's Wi-Fi, PHY and coexistence libraries are Apache-2.0 but ship
   only as binaries, so their source cannot be provided. This is the same position as every
   GPL-licensed ESP32 firmware. Removing it fully would need wolfSSL out of the build (an
   upstream CrossInk choice) or a commercial wolfSSL license.
2. **LPPL hyphenation patterns (v1.6 only).** The Russian, Swedish and Ukrainian patterns are
   LPPL-only. The FSF considers the LPPL free but GPL-incompatible. The patterns are compiled
   data tables, and their notices and the LPPL text ([`licenses/LPPL-1.3c.txt`](licenses/LPPL-1.3c.txt))
   ship with every release. In v2.0 they are not built in.
3. **MiniBidi.** Upstream marks it MIT, but it includes changes from mintty
   (GPL-3.0-or-later). In these GPL-3.0-or-later binaries that is compatible; it matters only
   for anyone who reuses the file under MIT alone.
