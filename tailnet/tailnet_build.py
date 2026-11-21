"""PlatformIO pre-script: layer Tailscale (MicroLink) onto an unmodified CrossInk build.

Wired in by the overlay's platformio.local.ini, which upstream already loads
(`extra_configs = platformio.local.ini`). It:

1. compiles MicroLink (an ESP-IDF component) as a static library against the
   framework's prebuilt ESP-IDF, with tailnet/ml_config.h standing in for Kconfig;
2. compiles the shim (tailnet/shim) straight into the program, so its
   initVariant() hook and lwip_getaddrinfo wrapper are always linked;
3. points the over-the-air updater at this overlay's releases, so an update
   never swaps the reader back to a build without Tailscale.
"""

import os

Import("env")  # noqa: F821  (SCons builtin)

OVERLAY_REPO = os.environ.get("TAILNET_OVERLAY_REPO", "Fledgewing/CrossInk-Tailnet")

# The overlay's tailnet/ directory is symlinked into the CrossInk checkout;
# resolve through the link to find the overlay root and the MicroLink submodule.
here = os.path.realpath(os.path.join(env.subst("$PROJECT_DIR"), "tailnet"))  # noqa: F821
overlay_root = os.path.dirname(here)
ml_dir = os.path.join(overlay_root, "third_party", "microlink", "components", "microlink")
wg_dir = os.path.join(ml_dir, "components", "wireguard_lwip", "src")
shim_dir = os.path.join(here, "shim")
config_h = os.path.join(here, "ml_config.h")

if not os.path.isfile(os.path.join(ml_dir, "include", "microlink.h")):
    raise RuntimeError(
        f"MicroLink not found at {ml_dir}; run `git submodule update --init` in the overlay repo"
    )

pioenv = env.subst("$PIOENV")  # noqa: F821
# Every CrossInk ESP32-S3 reader has 8 MB PSRAM; the ESP32-C3 (X3/X4) has none.
has_psram = env.BoardConfig().get("build.mcu", "") == "esp32s3"  # noqa: F821


# --- Per-device switches ---------------------------------------------------------
# ONE table for every memory/behaviour switch: one row per switch, one column
# per env. "default" is the X3 and the X4 (ESP32-C3, no PSRAM), "x4-pro" and
# "x4-classic" the ESP32-S3 readers (8 MB PSRAM); any other env takes its
# chip's column (C3 -> default, S3 -> x4-pro). None leaves the switch to its
# header default (#ifndef). Override one for a build with an environment
# variable of the same name, e.g. ML_TASK_TUNNEL_STACK=8192 pio run -e default.
# Where each value goes:
#   ML_* / CONFIG_ML_*      MicroLink and the shim only (ml_config.h territory);
#   WOLFSSL_SWITCH_DEFINES  the wolfSSL defines they stand for (whole build);
#   everything else         a -D for the whole build (CrossInk, MicroLink, shim).
# Rule: an improvement with no functional or performance downside is on for
# every env; a trade-off (smaller queues, stacks, caches, DERP only...) is on
# for "default" only.
SWITCH_ENVS = ("default", "x4-pro", "x4-classic")
SWITCHES = {
    #                                     default  x4-pro  x4-classic
    # --- MicroLink fork (small-memory client mode); CONFIG_ML_LITE is the X3's master default.
    "CONFIG_ML_LITE":                     (1,      None,   None),
    "ML_LITE_TASKS":                      (1,      0,      0),      # DERP + WireGuard in one task
    "ML_LITE_CONTEXT":                    (1,      0,      0),      # small microlink_t, few peers
    "ML_LITE_NO_DIRECT":                  (1,      0,      0),      # DERP only, no STUN/UDP
    "ML_LITE_SMALL_FILTER":               (1,      0,      0),
    "ML_LITE_DERP_WOLFSSL":               (1,      0,      0),      # DERP TLS on wolfSSL
    "ML_LITE_CONTROL_PLAIN":              (1,      0,      0),      # control on port 80, 443 fallback
    "ML_LITE_CONTROL_ONESHOT":            (1,      0,      0),      # control then close
    "ML_LITE_STREAM_MAP":                 (1,      0,      0),      # streamed MapResponse
    "ML_LITE_CACHED_START":               (1,      0,      0),      # skip control when cached state is valid
    "ML_LITE_DERP_AFTER_CONTROL":         (1,      0,      0),      # relay connects after the session closed
    "ML_LITE_REG_ONEBUF":                 (1,      1,      1),      # one 4 KB RegisterResponse buffer
    "ML_LITE_DERP_FRAME_CAP":             (4096,   4096,   4096),   # larger DERP frames drained, not allocated
    "ML_LITE_DERP_LAZY":                  (1,      0,      0),      # region table only when needed
    "ML_LITE_DERP_TRIM":                  (1,      0,      0),      # region table trimmed after a session
    "ML_LITE_RX_ONEBUF":                  (1,      0,      0),      # relayed packets: one buffer, decrypted in place
    "ML_LITE_SMALL_QUEUES":               (1,      0,      0),      # wg_rx 4, derp_tx 4, disco 1, peer_update 4
    "ML_LITE_NACL_CACHE_SLOTS":           (2,      0,      0),      # DISCO shared keys (0 = 24)
    "ML_LITE_MAX_PEERS":                  (1,      None,   None),   # peer table with ML_LITE_CONTEXT
    "ML_TASK_TUNNEL_STACK":               (5120,   None,   None),   # ~3.2 KB used after DERP TLS (X3 log)
    "CONFIG_ML_MAX_PEERS":                (8,      None,   None),
    "CONFIG_ML_H2_BUFFER_SIZE_KB":        (48,     None,   None),   # map buffers in internal RAM
    "CONFIG_ML_JSON_BUFFER_SIZE_KB":      (48,     None,   None),
    # --- System (shim/TailnetSysSwitches.h). Stack sizes are bytes, 0 = the framework's
    # own size; each comment gives the size as created and the X3's worst use over catalog,
    # download and sync (TAILNET_STACK_LOG report).
    "TAILNET_STACK_LOG":                  (1,      0,      0),      # measurement: task stack report
    "TAILNET_TASK_STACK_WRAP":            (1,      0,      0),      # links the size table below
    "TAILNET_STACK_ESP_TIMER":            (2048,   0,      0),      # 4608: X3 used ~0.4 KB
    "TAILNET_STACK_TCPIP":                (3072,   0,      0),      # 4608: X3 used ~1.2 KB
    "TAILNET_STACK_ARDUINO_EVENTS":       (0,      0,      0),      # 4096: X3 used ~2.3 KB, kept
    "TAILNET_STACK_SYS_EVT":              (2048,   0,      0),      # 4608: X3 used ~0.9 KB
    "TAILNET_STACK_WIFI":                 (0,      0,      0),      # driver's own size (not tunable)
    "TAILNET_STACK_TMR_SVC":              (1024,   0,      0),      # 2560: X3 used ~0.3 KB
    "TAILNET_LAZY_WORKER":                (1,      1,      1),      # tailnet worker only while needed
    "TAILNET_WORKER_STACK":               (4096,   6144,   6144),   # worker stack bytes; 6144: X3 used ~2.6 KB
    # --- WiFi driver init (shim/TailnetWifiMem.c). Buffer counts: 0 = the framework's
    # count; any count set also turns AMPDU TX off and caps the RX BA window.
    # Small tier: the live catalog (framebuffer kept) and every boot not listed below.
    "TAILNET_WIFI_STATIC_RX":             (2,      0,      0),
    "TAILNET_WIFI_DYNAMIC_RX":            (4,      0,      0),
    "TAILNET_WIFI_DYNAMIC_TX":            (4,      0,      0),
    "TAILNET_WIFI_RX_MGMT":               (3,      0,      0),      # RX mgmt buffers 5 -> 3 (0 = keep)
    # Parked tier: boots that park the framebuffer or draw nothing while WiFi is up
    # (syncs 8/9, download 10, Home refresh/shelves 12/13, login/auth 64/65), ~80-118 KB free.
    # All three counts set = the tier is on; AMPDU TX stays off.
    "TAILNET_WIFI_PARKED_STATIC_RX":      (3,      0,      0),
    "TAILNET_WIFI_PARKED_DYNAMIC_RX":     (8,      0,      0),
    "TAILNET_WIFI_PARKED_DYNAMIC_TX":     (8,      0,      0),
    "TAILNET_WIFI_PARKED_RX_MGMT":        (5,      0,      0),      # the framework's 5
    "TAILNET_WIFI_NO_NVS":                (1,      1,      1),      # WiFi driver config never in NVS
    "TAILNET_WIFI_ALLOC_COUNT":           (0,      0,      0),      # measurement: WiFi driver heap totals
    # --- Network boots (ui/TailnetNetPhase.cpp)
    "TAILNET_PROBE_CATALOG_LIVE":         (1,      None,   None),   # Tailscale in the catalog boot, screen kept
    "TAILNET_PARK_DURING_CONNECT":        (1,      0,      0),      # live catalog: cached start kept, else control parked + restart
    # --- CrossInk patches 0004 (memory cuts, include/CrossInkTailnetCuts.h) and 0005 (WiFi join).
    # BookOrbit sync: a new TLS session needs this much free heap (0 = upstream's 55000)...
    "CROSSINK_TAILNET_SYNC_TLS_FLOOR":    (35000,  35000,  35000),
    # ...and a largest free block this big (measured X3 handshake peaks: up to 24.0 KB).
    "CROSSINK_TAILNET_SYNC_TLS_BLOCK":    (24576,  24576,  24576),
    # Catalog bursts (open, Home refresh, shelf options) share one keep-alive connection.
    "CROSSINK_TAILNET_CATALOG_KEEPALIVE": (1,      1,      1),
    # ...and in the catalog boot (BOOKORBIT_CATALOG) one connection for the whole boot, not per
    # burst: its wolfSSL session sits in the TLS arena, so list screens reuse it instead of a fresh
    # handshake next to the resident UI. Inert on the S3 readers (no catalog boot there).
    "CROSSINK_TAILNET_CATALOG_BOOT_SESSION": (1,   1,      1),
    # Catalog JSON filters built after the download instead of during it.
    "CROSSINK_TAILNET_FILTER_AFTER_TLS":  (1,      1,      1),
    # wolfSSL (whole library): RSA public-key operations only (RsaKey 5368 -> ~2140 B).
    # Needs WOLFSSL_NO_CLIENT_AUTH, which drops the client-certificate signing code: the
    # reader never presents a client certificate (no wolfSSL_use_* call anywhere)...
    "CROSSINK_TAILNET_WOLFSSL_RSA_PUBLIC_ONLY": (1, 1,      1),
    # ...and no session cache (784 B of .bss): nothing calls wolfSSL_set_session, so the
    # client cache was written and never read. Needs MicroLink lite-xcuts (ml_derp.c
    # skips wolfSSL_CTX_set_session_cache_mode, which NO_SESSION_CACHE removes).
    "CROSSINK_TAILNET_WOLFSSL_NO_SESSION_CACHE": (1, 1,     1),
    # .bss moved to the heap until used: dictionary worker stack (4452 B), MiniBidi
    # buffers (3072 B), font prewarm scan array (2052 B).
    "CROSSINK_TAILNET_DICT_STACK_ON_DEMAND": (1,   0,      0),
    "CROSSINK_TAILNET_BIDI_ON_DEMAND":    (1,      0,      0),
    "CROSSINK_TAILNET_FONT_SCAN_ON_DEMAND": (1,    0,      0),
    # loopTask stack in a network boot, bytes (0 = FreeInkUI's 16 KB everywhere).
    "CROSSINK_TAILNET_NET_LOOP_STACK":    (10240,  0,      0),      # X3 catalog used ~8.6 KB: keep
    # Render task (ActivityManager) stack in a network boot, bytes (patch 0006; 0 = upstream's 8 KB).
    "CROSSINK_TAILNET_NET_RENDER_STACK":  (6144,   0,      0),      # 8192: X3 used ~2.6 KB
    # Catalog root: no entry-count requests unless cached. Off everywhere: with
    # keep-alive the counts cost round trips, not handshakes, and skipping them shows.
    "CROSSINK_TAILNET_SKIP_ROOT_COUNTS":  (0,      0,      0),
    # wolfSSL arena for BookOrbit/catalog requests, bytes (0 = off): one block reserved
    # when WiFi joins, before Tailscale (and before the framebuffer is parked, so it never
    # sits in the framebuffer's hole), so a fragmented heap cannot fail a handshake.
    "CROSSINK_TAILNET_TLS_ARENA":         (24576,  0,      0),
    "CROSSINK_TAILNET_WIFI_FAST_JOIN":    (1,      1,      1),      # patch 0005: last channel+BSSID first
    # Patch 0007: a download boot that ran with the network parked (the C3 with Tailscale)
    # restarts into the catalog at the listing it came from instead of ending on Home.
    # Inert on the S3 readers: nothing parks there, they download in place and stay in the catalog.
    "CROSSINK_TAILNET_DOWNLOAD_BACK_TO_CATALOG": (1, 1,     1),
}
# Switches that stand for wolfSSL defines rather than a define of their own.
WOLFSSL_SWITCH_DEFINES = {
    "CROSSINK_TAILNET_WOLFSSL_RSA_PUBLIC_ONLY": ["WOLFSSL_RSA_PUBLIC_ONLY", "WOLFSSL_NO_CLIENT_AUTH"],
    "CROSSINK_TAILNET_WOLFSSL_NO_SESSION_CACHE": ["NO_SESSION_CACHE"],
}
SWITCH_COLUMN = pioenv if pioenv in SWITCH_ENVS else ("x4-pro" if has_psram else "default")


def switch_value(name):
    """The switch's value for this build (env var, else its row); None = header default."""
    if name in os.environ:
        return int(os.environ[name], 0)
    return SWITCHES[name][SWITCH_ENVS.index(SWITCH_COLUMN)]


SWITCH_VALUES = {name: switch_value(name) for name in SWITCHES}
print(f"Tailnet switches for {pioenv} (column {SWITCH_COLUMN}):")
for _name, _value in SWITCH_VALUES.items():
    _src = " (env override)" if _name in os.environ else ""
    print(f"  {_name:<42} {'header default' if _value is None else _value}{_src}")

ML_DEFINES = []       # MicroLink + shim only (appended in _process_project_deps_with_tailnet)
_global_defines = []  # the whole build
for _name, _value in SWITCH_VALUES.items():
    if _value is None:
        continue
    if _name in WOLFSSL_SWITCH_DEFINES:
        if _value:
            _global_defines += WOLFSSL_SWITCH_DEFINES[_name]
    elif _name.startswith(("ML_", "CONFIG_ML_")):
        ML_DEFINES.append((_name, _value))
    else:
        _global_defines.append((_name, _value))
env.Append(CPPDEFINES=_global_defines)  # noqa: F821

# The C3's DERP relay TLS runs on wolfSSL (ML_LITE_DERP_WOLFSSL): the application already compiles Arduino-wolfSSL from
# source, and its session is ~5 KB once connected against the prebuilt
# mbedTLS's ~37 KB. Verifying Tailscale's relays (Let's Encrypt ECDSA chains,
# signed with ecdsa-with-SHA384 by P-384 keys) needs SHA-384, which that build
# leaves out, and SP math for P-384 keeps the handshake peak at ~24 KB instead
# of ~43 KB with fast-math bignums. These apply to the whole wolfSSL library.
derp_wolfssl = bool(SWITCH_VALUES.get("ML_LITE_DERP_WOLFSSL"))
if derp_wolfssl:
    env.Append(CPPDEFINES=["WOLFSSL_SHA384", "WOLFSSL_SHA512", "WOLFSSL_SP_384"])  # noqa: F821

# --- 1. MicroLink as a library ------------------------------------------------

# Same source list as MicroLink's CMakeLists.txt. Cellular, failover and the
# config web server compile to stubs when their CONFIG_ML_* switch is off.
ML_SRCS = [
    "microlink.c", "ml_coord.c", "ml_coord_tls.c", "ml_map_stream.c", "ml_derp.c", "ml_net_io.c",
    "ml_wg_mgr.c", "ml_stun.c", "ml_noise.c", "ml_h2.c", "ml_udp.c", "ml_filter.c",
    "ml_routes.c", "ml_nat66.c", "ml_ota.c", "ml_tcp.c", "ml_peer_nvs.c",
    "ml_zerocopy.c", "ml_cellular.c", "ml_at_socket.c", "ml_at_validate.c",
    "ml_net_switch.c", "ml_config_httpd.c",
]
WG_SRCS = ["wireguard.c", "wireguardif.c", "wireguard-filter.c", "crypto.c", "wireguard-platform-esp32.c"]
WG_CRYPTO = ["blake2s.c", "chacha20.c", "chacha20poly1305.c", "poly1305-donna.c", "x25519.c"]

# Framework include paths and flags are only set inside BuildProgram()
# (ProcessProgramDeps -> BuildFrameworks), after pre-scripts run. Hook the next
# step, ProcessProjectDeps, so the clones below inherit the full environment and
# our objects still reach the program's link.
_orig_process_project_deps = env.ProcessProjectDeps  # noqa: F821


def _process_project_deps_with_tailnet(env):
    _orig_process_project_deps()
    ml_env = env.Clone()
    ml_env.Append(
        CPPPATH=[os.path.join(ml_dir, "include"), os.path.join(ml_dir, "src"), wg_dir,
                 os.path.join(wg_dir, "crypto", "refc")],
        # The framework compiles logs below ERROR out; MicroLink and the shim
        # keep INFO so TailnetLog.cpp can show them on the Tailscale screen.
        CPPDEFINES=["WIREGUARD_CRYPTO_REFC", ("LOG_LOCAL_LEVEL", 3)],
        CCFLAGS=["-include", config_h, "-Wno-stringop-overread"],
    )
    # Per-device switches (SWITCHES above). The peer table stays small
    # (ml_config.h: 16; 8 on the C3): the reader only needs its servers, and
    # tailnet_ensure_peer() pins one that a large tailnet would otherwise leave out.
    ml_env.Append(CPPDEFINES=ML_DEFINES)

    if derp_wolfssl:
        # ml_derp.c includes wolfSSL; the defines (WOLFSSL_USER_SETTINGS, ...)
        # come with the cloned environment, the headers from its library.
        wolfssl_src = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), pioenv, "Arduino-wolfSSL", "src")
        if not os.path.isfile(os.path.join(wolfssl_src, "wolfssl", "ssl.h")):
            raise RuntimeError(f"wolfSSL not found at {wolfssl_src}; the DERP relay TLS needs Arduino-wolfSSL")
        ml_env.Append(CPPPATH=[wolfssl_src])

    build_dir = os.path.join(env.subst("$BUILD_DIR"), "tailnet")
    objs = []
    for name in ML_SRCS:
        objs.append(ml_env.Object(os.path.join(build_dir, "ml", name.replace(".c", ".o")),
                                  os.path.join(ml_dir, "src", name)))
    for name in WG_SRCS:
        objs.append(ml_env.Object(os.path.join(build_dir, "wg", name.replace(".c", ".o")),
                                  os.path.join(wg_dir, name)))
    hot_env = ml_env.Clone()
    hot_env.Append(CCFLAGS=["-O2"])  # WireGuard + DISCO crypto, as MicroLink's CMake does
    for name in WG_CRYPTO:
        objs.append(hot_env.Object(os.path.join(build_dir, "wg", name.replace(".c", ".o")),
                                   os.path.join(wg_dir, "crypto", "refc", name)))
    objs.append(hot_env.Object(os.path.join(build_dir, "ml", "nacl_box.o"),
                               os.path.join(ml_dir, "src", "nacl_box.c")))

    microlink_lib = ml_env.StaticLibrary(os.path.join(build_dir, "microlink"), objs)

    # --- 2. The shim, linked directly ----------------------------------------------

    shim_env = ml_env.Clone()
    # Arduino library headers reach only the library builders that the LDF
    # picked; the shim needs WiFi (and Network, which WiFi.h includes).
    fw_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    shim_env.Append(CPPPATH=[os.path.join(fw_dir, "libraries", lib, "src") for lib in ("WiFi", "Network")])
    shim_objs = [
        shim_env.Object(os.path.join(build_dir, "shim", os.path.splitext(f)[0] + ".o"), os.path.join(shim_dir, f))
        for f in sorted(os.listdir(shim_dir)) if f.endswith((".cpp", ".c"))
    ]
    env.Append(PIOBUILDFILES=shim_objs)
    env.Append(LIBS=[microlink_lib])
    env.Append(LINKFLAGS=["-Wl,--wrap=lwip_getaddrinfo", "-Wl,--wrap=lwip_connect",
                          # TailnetLog.cpp: keep our log lines past the
                          # prebuilt ERROR-only log level
                          "-Wl,--wrap=esp_log",
                          # TailnetTlsMem.c: mbedTLS buffers in PSRAM
                          "-Wl,--wrap=esp_mbedtls_mem_calloc",
                          # TailnetWifiMem.c: the WiFi init switches
                          "-Wl,--wrap=esp_wifi_init"])
    if SWITCH_VALUES.get("TAILNET_TASK_STACK_WRAP"):
        # TailnetStacks.c: task stack sizes by name, and the timer task's
        env.Append(LINKFLAGS=["-Wl,--wrap=xTaskCreatePinnedToCore",
                              "-Wl,--wrap=vApplicationGetTimerTaskMemory"])
    if SWITCH_VALUES.get("TAILNET_STACK_LOG"):
        # TailnetStacks.c: the stack report before WiFi goes off
        env.Append(LINKFLAGS=["-Wl,--wrap=esp_wifi_stop"])


env.AddMethod(_process_project_deps_with_tailnet, "ProcessProjectDeps")  # noqa: F821
env.Append(CPPPATH=[shim_dir])  # noqa: F821  lets optional UI code #include "Tailnet.h"

# --- 3. OTA from this overlay's releases -----------------------------------------

ota_defines = {
    "CROSSINK_OTA_RELEASE_URL": f"https://api.github.com/repos/{OVERLAY_REPO}/releases/latest",
    "CROSSINK_OTA_RELEASE_LIST_URL": f"https://api.github.com/repos/{OVERLAY_REPO}/releases?per_page=5",
}
flags = env.get("BUILD_FLAGS", [])  # noqa: F821
flags = [f for f in flags if not any(f.startswith(f"-D{k}=") for k in ota_defines)]
flags += [f'-D{k}=\\"{v}\\"' for k, v in ota_defines.items()]
env.Replace(BUILD_FLAGS=flags)  # noqa: F821

print(f"Tailnet overlay: MicroLink + shim for {pioenv} (PSRAM: {has_psram}); OTA from {OVERLAY_REPO}")
