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
# ONE table for every memory/behaviour switch, one row per switch, one column
# per env: "default" is the X3 and the X4 (ESP32-C3, no PSRAM), "x4-pro" and
# "x4-classic" the ESP32-S3 readers (8 MB PSRAM). None leaves the switch to its
# header default. Override one for a build with an environment variable of
# the same name, e.g. ML_TASK_TUNNEL_STACK=8192 pio run -e default.
# Switches named ML_*/CONFIG_ML_* reach MicroLink and the shim only; the rest
# (TAILNET_*, CROSSINK_TAILNET_*) the whole program too.
# Rule: an improvement with no functional or performance downside is on for
# every env; a trade-off (smaller queues, stacks, caches, DERP only...) is on
# for "default" only.
SWITCHES = {
    # Small-memory client mode (MicroLink fork): the X3's master default.
    "CONFIG_ML_LITE":                {"default": 1,    "x4-pro": None, "x4-classic": None},
    "ML_LITE_TASKS":                 {"default": 1,    "x4-pro": 0,    "x4-classic": 0},     # DERP + WireGuard in one task
    "ML_LITE_CONTEXT":               {"default": 1,    "x4-pro": 0,    "x4-classic": 0},     # small microlink_t, few peers
    "ML_LITE_NO_DIRECT":             {"default": 1,    "x4-pro": 0,    "x4-classic": 0},     # DERP only, no STUN/UDP
    "ML_LITE_SMALL_FILTER":          {"default": 1,    "x4-pro": 0,    "x4-classic": 0},
    "ML_LITE_DERP_WOLFSSL":          {"default": 1,    "x4-pro": 0,    "x4-classic": 0},     # DERP TLS on wolfSSL
    "ML_LITE_CONTROL_PLAIN":         {"default": 1,    "x4-pro": 0,    "x4-classic": 0},     # control on port 80, 443 fallback
    "ML_LITE_CONTROL_ONESHOT":       {"default": 1,    "x4-pro": 0,    "x4-classic": 0},     # control then close
    "ML_LITE_STREAM_MAP":            {"default": 1,    "x4-pro": 0,    "x4-classic": 0},     # streamed MapResponse
    "ML_LITE_CACHED_START":          {"default": 1,    "x4-pro": 0,    "x4-classic": 0},     # skip control when cached state is valid
    "ML_LITE_DERP_AFTER_CONTROL":    {"default": 1,    "x4-pro": 0,    "x4-classic": 0},     # relay connects after the session closed
    "ML_LITE_REG_ONEBUF":            {"default": 1,    "x4-pro": 1,    "x4-classic": 1},     # one 4 KB RegisterResponse buffer
    "ML_LITE_DERP_FRAME_CAP":        {"default": 4096, "x4-pro": 4096, "x4-classic": 4096},  # larger DERP frames drained, not allocated
    "ML_LITE_DERP_LAZY":             {"default": 1,    "x4-pro": 0,    "x4-classic": 0},     # region table only when needed
    "ML_LITE_DERP_TRIM":             {"default": 1,    "x4-pro": 0,    "x4-classic": 0},     # region table trimmed after a session
    "ML_LITE_RX_ONEBUF":             {"default": 1,    "x4-pro": 0,    "x4-classic": 0},     # relayed packets: one buffer, decrypted in place
    "ML_LITE_SMALL_QUEUES":          {"default": 1,    "x4-pro": 0,    "x4-classic": 0},     # wg_rx 4, derp_tx 4, disco 1, peer_update 4
    "ML_LITE_NACL_CACHE_SLOTS":      {"default": 2,    "x4-pro": 0,    "x4-classic": 0},     # DISCO shared keys (0 = 24)
    "ML_LITE_MAX_PEERS":             {"default": 1,    "x4-pro": None, "x4-classic": None},  # peer table with ML_LITE_CONTEXT
    "ML_TASK_TUNNEL_STACK":          {"default": 5120, "x4-pro": None, "x4-classic": None},  # ~3.2 KB used after DERP TLS (X3 log)
    "CONFIG_ML_MAX_PEERS":           {"default": 8,    "x4-pro": None, "x4-classic": None},
    "CONFIG_ML_H2_BUFFER_SIZE_KB":   {"default": 48,   "x4-pro": None, "x4-classic": None},  # map buffers in internal RAM
    "CONFIG_ML_JSON_BUFFER_SIZE_KB": {"default": 48,   "x4-pro": None, "x4-classic": None},
    # WiFi driver buffers (tailnet/shim/TailnetWifiMem.c, CONFIG_ML_LITE only)
    "TAILNET_WIFI_STATIC_RX":        {"default": 2,    "x4-pro": None, "x4-classic": None},
    "TAILNET_WIFI_DYNAMIC_RX":       {"default": 4,    "x4-pro": None, "x4-classic": None},
    "TAILNET_WIFI_DYNAMIC_TX":       {"default": 4,    "x4-pro": None, "x4-classic": None},
    # Catalog probe: Tailscale in the catalog boot with the screen kept
    "TAILNET_PROBE_CATALOG_LIVE":    {"default": 1,    "x4-pro": None, "x4-classic": None},
}


def _switch_values():
    col = pioenv if pioenv in ("default", "x4-pro", "x4-classic") else ("x4-pro" if has_psram else "default")
    values = {}
    for name, row in SWITCHES.items():
        v = row.get(col)
        if name in os.environ:
            v = int(os.environ[name])
        if v is not None:
            values[name] = v
    return values


SWITCH_VALUES = _switch_values()
print("Tailnet switches (" + pioenv + "): " + ", ".join(f"{k}={v}" for k, v in SWITCH_VALUES.items()))
ML_DEFINES = [(k, v) for k, v in SWITCH_VALUES.items() if k.startswith(("ML_", "CONFIG_ML_", "TAILNET_WIFI_"))]
env.Append(CPPDEFINES=[(k, v) for k, v in SWITCH_VALUES.items() if (k, v) not in ML_DEFINES])  # noqa: F821

# The C3's DERP relay TLS runs on wolfSSL (ML_LITE_DERP_WOLFSSL, see SWITCHES): the application already compiles Arduino-wolfSSL from
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
                          "-Wl,--wrap=esp_mbedtls_mem_calloc"])
    if SWITCH_VALUES.get("CONFIG_ML_LITE"):
        # TailnetWifiMem.c: fewer WiFi driver buffers (CONFIG_ML_LITE)
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
