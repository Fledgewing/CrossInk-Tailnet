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

# The C3's DERP relay TLS runs on wolfSSL (ML_LITE_DERP_WOLFSSL, on with
# CONFIG_ML_LITE below): the application already compiles Arduino-wolfSSL from
# source, and its session is ~5 KB once connected against the prebuilt
# mbedTLS's ~37 KB. Verifying Tailscale's relays (Let's Encrypt ECDSA chains,
# signed with ecdsa-with-SHA384 by P-384 keys) needs SHA-384, which that build
# leaves out, and SP math for P-384 keeps the handshake peak at ~24 KB instead
# of ~43 KB with fast-math bignums. These apply to the whole wolfSSL library.
derp_wolfssl = not has_psram
if derp_wolfssl:
    env.Append(CPPDEFINES=["WOLFSSL_SHA384", "WOLFSSL_SHA512", "WOLFSSL_SP_384"])  # noqa: F821

# --- Per-device switches ----------------------------------------------------------
# One row per switch: its value for each env (default = X3/X4 on the ESP32-C3,
# x4-pro, x4-classic). An environment variable of the same name overrides the
# row for one build, e.g. CROSSINK_TAILNET_NET_LOOP_STACK=0 pio run -e default.
# Pure improvements are on everywhere; trade-offs are on for the C3 only.
SWITCH_ENVS = ("default", "x4-pro", "x4-classic")
SWITCHES = {
    # CrossInk patch 0004 (memory cuts); the C side reads include/CrossInkTailnetCuts.h.
    # BookOrbit sync: a new TLS session needs this much free heap (0 = upstream's 55000)...
    "CROSSINK_TAILNET_SYNC_TLS_FLOOR": (35000, 35000, 35000),
    # ...and a largest free block this big (measured X3 handshake peaks: up to 24.0 KB).
    "CROSSINK_TAILNET_SYNC_TLS_BLOCK": (24576, 24576, 24576),
    # Catalog bursts (open, Home refresh, shelf options) share one keep-alive connection.
    "CROSSINK_TAILNET_CATALOG_KEEPALIVE": (1, 1, 1),
    # Catalog JSON filters built after the download instead of during it.
    "CROSSINK_TAILNET_FILTER_AFTER_TLS": (1, 1, 1),
    # wolfSSL (whole library): RSA public-key operations only (RsaKey 5368 -> ~2140 B).
    # Needs WOLFSSL_NO_CLIENT_AUTH, which drops the client-certificate signing code: the
    # reader never presents a client certificate (no wolfSSL_use_* call anywhere)...
    "CROSSINK_TAILNET_WOLFSSL_RSA_PUBLIC_ONLY": (1, 1, 1),
    # ...and no session cache (784 B of .bss): nothing calls wolfSSL_set_session, so the
    # client cache was written and never read. Needs MicroLink lite-xcuts (ml_derp.c
    # skips wolfSSL_CTX_set_session_cache_mode, which NO_SESSION_CACHE removes).
    "CROSSINK_TAILNET_WOLFSSL_NO_SESSION_CACHE": (1, 1, 1),
    # .bss moved to the heap until used: dictionary worker stack (4452 B), MiniBidi
    # buffers (3072 B), font prewarm scan array (2052 B).
    "CROSSINK_TAILNET_DICT_STACK_ON_DEMAND": (1, 0, 0),
    "CROSSINK_TAILNET_BIDI_ON_DEMAND": (1, 0, 0),
    "CROSSINK_TAILNET_FONT_SCAN_ON_DEMAND": (1, 0, 0),
    # loopTask stack in a network boot, bytes (0 = FreeInkUI's 16 KB everywhere).
    "CROSSINK_TAILNET_NET_LOOP_STACK": (10240, 0, 0),
    # Catalog root: no entry-count requests unless cached.
    "CROSSINK_TAILNET_SKIP_ROOT_COUNTS": (1, 0, 0),
    # wolfSSL arena for BookOrbit/catalog requests, bytes (0 = off): one block reserved
    # when WiFi joins, before Tailscale, so a fragmented heap cannot fail a handshake.
    "CROSSINK_TAILNET_TLS_ARENA": (24576, 0, 0),
}
# Switches that are a wolfSSL define rather than a CrossInk one.
WOLFSSL_SWITCH_DEFINES = {
    "CROSSINK_TAILNET_WOLFSSL_RSA_PUBLIC_ONLY": ["WOLFSSL_RSA_PUBLIC_ONLY", "WOLFSSL_NO_CLIENT_AUTH"],
    "CROSSINK_TAILNET_WOLFSSL_NO_SESSION_CACHE": ["NO_SESSION_CACHE"],
}


def switch_value(name):
    if name in os.environ:
        return int(os.environ[name], 0)
    column = SWITCH_ENVS.index(pioenv) if pioenv in SWITCH_ENVS else 0
    return SWITCHES[name][column]


_switch_defines = []
for _name in SWITCHES:
    _value = switch_value(_name)
    if _name in WOLFSSL_SWITCH_DEFINES:
        if _value:
            _switch_defines += WOLFSSL_SWITCH_DEFINES[_name]
    else:
        _switch_defines.append((_name, _value))
env.Append(CPPDEFINES=_switch_defines)  # noqa: F821  CrossInk, its libraries and wolfSSL
print("Tailnet switches (" + pioenv + "): " + ", ".join(f"{_n}={switch_value(_n)}" for _n in SWITCHES))

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


# --- Catalog probe (branch c3-catalog-probe only, C3 only) ----------------------
# Each cut has its own switch: set the environment variable to 0 to turn it
# off, e.g. TAILNET_PROBE_TUNNEL_STACK=0 pio run -e default.
def _probe_switch(name, default="1"):
    return os.environ.get(name, default) != "0"


PROBE_DEFINES = []
if not has_psram:
    if _probe_switch("TAILNET_PROBE_CATALOG_LIVE"):  # Tailscale in the catalog boot, screen kept
        PROBE_DEFINES.append(("TAILNET_PROBE_CATALOG_LIVE", 1))
    if _probe_switch("TAILNET_PROBE_TUNNEL_STACK"):  # a. tunnel task stack 8192 -> 5120
        PROBE_DEFINES.append(("ML_TASK_TUNNEL_STACK", 5120))
    if _probe_switch("TAILNET_PROBE_DERP_TRIM"):  # b. DERP region table: home + peers' regions only
        PROBE_DEFINES.append(("ML_LITE_DERP_TRIM", 1))
    if _probe_switch("TAILNET_PROBE_WIFI_SMALL"):  # c. WiFi static RX 2, dynamic RX 4, dynamic TX 4
        PROBE_DEFINES += [("TAILNET_WIFI_STATIC_RX", 2), ("TAILNET_WIFI_DYNAMIC_RX", 4), ("TAILNET_WIFI_DYNAMIC_TX", 4)]
    if _probe_switch("TAILNET_PROBE_ONE_PEER"):  # d. peer table 2 -> 1
        PROBE_DEFINES.append(("ML_LITE_MAX_PEERS", 1))
    print("Tailnet catalog probe switches: " + ", ".join(f"{k}={v}" for k, v in PROBE_DEFINES))
env.Append(CPPDEFINES=[d for d in PROBE_DEFINES if d[0] == "TAILNET_PROBE_CATALOG_LIVE"])  # noqa: F821  CrossInk + ui


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
    # The peer table stays small (ml_config.h: 16; 8 here on the C3): the reader
    # only needs its servers, and tailnet_ensure_peer() pins one that a large
    # tailnet would otherwise leave out.
    if not has_psram:
        # No PSRAM: the map-response buffers come from internal RAM.
        ml_env.Append(CPPDEFINES=[("CONFIG_ML_H2_BUFFER_SIZE_KB", 48), ("CONFIG_ML_JSON_BUFFER_SIZE_KB", 48),
                                  ("CONFIG_ML_MAX_PEERS", 8),
                                  # The fork's small-memory client mode (Fledgewing/microlink,
                                  # crossink branch): the X3 has ~58 KB free once WiFi is up.
                                  ("CONFIG_ML_LITE", 1)])
        ml_env.Append(CPPDEFINES=PROBE_DEFINES)

    if derp_wolfssl:
        # ml_derp.c includes wolfSSL; the defines (WOLFSSL_USER_SETTINGS, ...)
        # come with the cloned environment, the headers from its library.
        wolfssl_src = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), pioenv, "Arduino-wolfSSL", "src")
        if not os.path.isfile(os.path.join(wolfssl_src, "wolfssl", "ssl.h")):
            raise RuntimeError(f"wolfSSL not found at {wolfssl_src}; the DERP relay TLS needs Arduino-wolfSSL")
        ml_env.Append(CPPPATH=[wolfssl_src], CPPDEFINES=[("ML_LITE_DERP_WOLFSSL", 1)])

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
                          "-Wl,--wrap=esp_mbedtls_mem_calloc"])
    if not has_psram:
        # TailnetWifiMem.c: fewer WiFi driver buffers (C3 only, CONFIG_ML_LITE)
        env.Append(LINKFLAGS=["-Wl,--wrap=esp_wifi_init"])


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
