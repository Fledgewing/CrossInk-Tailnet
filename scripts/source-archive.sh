#!/usr/bin/env bash
# Pack the complete corresponding source of a firmware release into one tarball.
#   scripts/source-archive.sh [--overlay DIR] --upstream DIR --version V --out FILE.tar.gz [--core DIR]
# --upstream: the CrossInk-Bookorbit checkout (with submodules) after scripts/apply.sh and
#   `pio pkg install` for every env, so .pio/libdeps/<env> holds the exact library sources.
# --core: the platform's framework-arduinoespressif32 package (the Arduino core source).
# --overlay: the overlay checkout to pack (default: this repository), with the MicroLink
#   submodule checked out.
# Used by .github/workflows/build.yml; nothing is built.
set -euo pipefail

overlay="$(cd "$(dirname "$0")/.." && pwd)"
upstream="" version="" out="" core=""
while [ $# -gt 0 ]; do
  case "$1" in
    --overlay) overlay="$(cd "$2" && pwd)"; shift 2 ;;
    --upstream) upstream="$(cd "$2" && pwd)"; shift 2 ;;
    --version) version="$2"; shift 2 ;;
    --out) out="$2"; shift 2 ;;
    --core) core="$(cd "$2" && pwd)"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[ -n "$upstream" ] && [ -n "$version" ] && [ -n "$out" ] \
  || { echo "usage: source-archive.sh [--overlay DIR] --upstream DIR --version V --out FILE [--core DIR]" >&2; exit 2; }
case "$out" in /*) ;; *) out="$PWD/$out" ;; esac

name="source-$(printf '%s' "$version" | sed 's/[^A-Za-z0-9._-]\{1,\}/-/g')"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
stage="$work/$name"
mkdir -p "$stage/overlay" "$stage/crossink"

# Tracked files only (the working tree content, so applied patches are included), submodules too.
copy_tracked() { # <repo> <dest> [exclude-prefix]
  git -C "$1" ls-files -z --recurse-submodules \
    | { if [ -n "${3:-}" ]; then grep -zv "^$3"; else cat; fi; } \
    | (cd "$1" && tar --null -T - -cf -) | tar -C "$2" -xf -
}
copy_tracked "$overlay" "$stage/overlay"
# Tabler's SVG sources are a large submodule; only icons converted into src/ are compiled.
copy_tracked "$upstream" "$stage/crossink" assets/tabler-icons/

# What apply.sh adds outside git: the PlatformIO config and two symlinks into the overlay.
cp "$upstream/platformio.local.ini" "$stage/crossink/"
ln -s ../overlay/tailnet "$stage/crossink/tailnet"
ln -s ../../overlay/tailnet/ui "$stage/crossink/src/tailnet"

# Library sources exactly as PlatformIO fetched them. Libraries identical to the first env's
# copy become symlinks to it; *.pio-link entries point into the tree and are recreated by pio.
libdeps="$upstream/.pio/libdeps"
first=""
for envdir in "$libdeps"/*/; do
  env="$(basename "$envdir")"
  mkdir -p "$stage/crossink/.pio/libdeps/$env"
  for lib in "$envdir"*/; do
    lib="${lib%/}"; n="$(basename "$lib")"
    case "$n" in *.pio-link) continue ;; esac
    if [ -n "$first" ] && [ -d "$libdeps/$first/$n" ] \
        && diff -rq -x .git "$lib" "$libdeps/$first/$n" >/dev/null 2>&1; then
      ln -s "../$first/$n" "$stage/crossink/.pio/libdeps/$env/$n"
    else
      (cd "$envdir" && tar --exclude .git -cf - "$n") | tar -C "$stage/crossink/.pio/libdeps/$env" -xf -
    fi
  done
  [ -n "$first" ] || first="$env"
done
[ -n "$first" ] || { echo "no libraries in $libdeps; run pio pkg install first" >&2; exit 1; }

core_version=""
if [ -n "$core" ]; then
  mkdir -p "$stage/framework-arduinoespressif32"
  (cd "$core" && tar -cf - cores libraries variants $(ls | grep -E '\.(txt|json|yml|projbuild)$|^CMakeLists\.txt$')) \
    | tar -C "$stage/framework-arduinoespressif32" -xf -
  core_version="$(sed -nE 's/^ *"version": *"([^"]+)".*/\1/p' "$core/package.json" | head -1)"
fi

platform="$(sed -nE 's#^platform *= *(https://[^ ]+)#\1#p' "$upstream/platformio.ini" | head -1)"
platform_ver="$(printf '%s' "$platform" | sed -nE 's#.*/download/([^/]+)/.*#\1#p')"
up_desc="$(git -C "$upstream" describe --tags --exact-match 2>/dev/null || git -C "$upstream" rev-parse HEAD)"
tabler="$(git -C "$upstream" ls-tree HEAD assets/tabler-icons | awk '{print $3}')"
microlink="$(git -C "$overlay" rev-parse HEAD:third_party/microlink)"
sdk="$(git -C "$upstream" ls-tree HEAD freeink-sdk | awk '{print $3}')"

cat > "$stage/BUILD.md" <<EOF
# Corresponding source: CrossInk-Tailnet $version

This archive is the complete corresponding source of the CrossInk-Tailnet firmware
$version (\`firmware-*.bin\`), which is distributed under GPL-3.0-or-later. See
\`overlay/THIRD_PARTY_LICENSES.md\` and \`overlay/licenses/\` for every component's license.

| Directory | What | Version |
|---|---|---|
| \`overlay/\` | CrossInk-Tailnet overlay | $(git -C "$overlay" rev-parse HEAD) |
| \`overlay/third_party/microlink/\` | MicroLink | $microlink |
| \`crossink/\` | CrossInk-Bookorbit after \`overlay/scripts/apply.sh\` (patches applied, release version set in \`platformio.ini\`) | $up_desc |
| \`crossink/freeink-sdk/\` | FreeInk SDK | $sdk |
| \`crossink/.pio/libdeps/<env>/\` | PlatformIO libraries as fetched for each reader env (a symlink means the same as \`$first\`) | see \`lib_deps\` in \`crossink/platformio.ini\` |
| \`framework-arduinoespressif32/\` | Arduino core source (LGPL-2.1-or-later), as packaged by the platform | ${core_version:-not included} |

Not included, all public:

- The platform, prebuilt ESP-IDF libraries and toolchain: pioarduino platform-espressif32
  $platform_ver, $platform
  (release page: https://github.com/pioarduino/platform-espressif32/releases/tag/$platform_ver).
  PlatformIO installs them from \`crossink/platformio.ini\`. ESP-IDF source: https://github.com/espressif/esp-idf
- Tabler Icons SVG sources (MIT; converted icons are in \`crossink/src/components/icons\`):
  https://github.com/tabler/tabler-icons/tree/$tabler

## Build

Needs PlatformIO Core 6.1.19 from pioarduino
(\`pip install https://github.com/pioarduino/platformio-core/archive/refs/tags/v6.1.19.zip\`).

\`\`\`bash
cd crossink
pio run -e x4-pro        # X4 Pro; x4-classic for the X4 Classic, default for the X3 / X4
\`\`\`

The overlay is already applied; \`crossink/tailnet\` and \`crossink/src/tailnet\` are
symlinks into \`overlay/\`. The image is \`.pio/build/<env>/firmware-*.bin\`. CrossInk 1.6
and 2.x use different platforms; keep a separate \`PLATFORMIO_CORE_DIR\` for each.
EOF

mkdir -p "$(dirname "$out")"
tar -C "$work" -czf "$out" "$name"
ls -l "$out"
