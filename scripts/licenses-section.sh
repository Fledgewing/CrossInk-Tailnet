#!/usr/bin/env bash
# Print the "Licenses and source" section of a release's notes.
#   scripts/licenses-section.sh <upstream-tag> <overlay-commit>
# Reads the MicroLink submodule pin and URL from this repository at <overlay-commit>
# (needs that commit locally), and the upstream tag's freeink-sdk pin and platform from
# GitHub with `gh` (GH_TOKEN). Env: OVERLAY_REPO (default Fledgewing/CrossInk-Tailnet),
# UPSTREAM_REPO (default agosez/CrossInk-Bookorbit).
# Used by .github/workflows/build.yml for new releases and by hand for older ones.
set -euo pipefail

tag="${1:?usage: licenses-section.sh <upstream-tag> <overlay-commit>}"
overlay="${2:?usage: licenses-section.sh <upstream-tag> <overlay-commit>}"
overlay_repo="${OVERLAY_REPO:-Fledgewing/CrossInk-Tailnet}"
upstream_repo="${UPSTREAM_REPO:-agosez/CrossInk-Bookorbit}"
here="$(cd "$(dirname "$0")/.." && pwd)"

enc() { printf '%s' "$1" | sed 's/+/%2B/g'; }
tag_enc="$(enc "$tag")"

ml_sha="$(git -C "$here" rev-parse "$overlay:third_party/microlink")"
ml_url="$(git -C "$here" show "$overlay:.gitmodules" \
  | git config -f /dev/stdin --get submodule.third_party/microlink.url)"
ml_url="${ml_url%.git}"

sdk_sha="$(gh api "repos/$upstream_repo/contents/freeink-sdk?ref=$tag_enc" -q .sha 2>/dev/null || true)"
platform="$(gh api "repos/$upstream_repo/contents/platformio.ini?ref=$tag_enc" \
  -H 'Accept: application/vnd.github.raw' 2>/dev/null \
  | sed -nE 's#^platform *= *(https://github.com/pioarduino/platform-espressif32/releases/download/([^/]+)/.*)#\2 \1#p' \
  | head -1 || true)"

cat <<EOF
## Licenses and source

The .bin files combine CrossInk-Bookorbit, MicroLink, the CrossInk-Tailnet overlay, the Arduino
core, ESP-IDF and other third-party components, including wolfSSL (GPL-2.0-or-later). They are
distributed under the **GNU General Public License v3.0 or later**. Every component, with its
license and copyright, is listed in \`THIRD_PARTY_LICENSES.md\`; the license texts and notices are
in \`licenses.zip\`. Both are attached to this release.

Complete corresponding source:

- CrossInk-Tailnet overlay: [\`${overlay:0:7}\`](https://github.com/$overlay_repo/tree/$overlay)
  (build recipe: [\`.github/workflows/build.yml\`](https://github.com/$overlay_repo/blob/$overlay/.github/workflows/build.yml),
  instructions: [Building locally](https://github.com/$overlay_repo/blob/$overlay/README.md#building-locally))
- CrossInk-Bookorbit: [\`$tag\`](https://github.com/$upstream_repo/tree/$tag_enc)$(
  [ -n "$sdk_sha" ] && printf ', with FreeInk SDK [`%s`](https://github.com/Free-Ink/freeink-sdk/tree/%s)' "${sdk_sha:0:7}" "$sdk_sha")
- MicroLink: [\`${ml_sha:0:7}\`]($ml_url/tree/$ml_sha)
- Libraries: the exact versions and commits in \`lib_deps\` of the tag's [\`platformio.ini\`](https://github.com/$upstream_repo/blob/$tag_enc/platformio.ini)$(
  [ -n "$platform" ] && printf '\n- Platform: [pioarduino platform-espressif32 %s](%s) (Arduino core, prebuilt ESP-IDF, toolchain), PlatformIO Core 6.1.19' "${platform%% *}" "${platform#* }")
EOF
