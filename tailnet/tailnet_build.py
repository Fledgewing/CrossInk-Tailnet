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

# --- 1. MicroLink as a library ------------------------------------------------

# Same source list as MicroLink's CMakeLists.txt. Cellular, failover and the
# config web server compile to stubs when their CONFIG_ML_* switch is off.
ML_SRCS = [
    "microlink.c", "ml_coord.c", "ml_coord_tls.c", "ml_derp.c", "ml_net_io.c",
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
