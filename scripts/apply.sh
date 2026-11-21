#!/usr/bin/env bash
# Layer the Tailnet overlay onto a CrossInk-Bookorbit checkout.
#   scripts/apply.sh /path/to/CrossInk-Bookorbit
# Idempotent. Touches only git-ignored paths in the checkout:
#   platformio.local.ini  (copied from overlay/)
#   tailnet               (symlink to this repo's tailnet/, excluded via .git/info/exclude)
#   src/tailnet           (symlink to tailnet/ui, built with CrossInk's own sources)
# and applies the patch series for its CrossInk version (overlay/patches/v2.0 or
# v1.6; the Settings row; on the C3, the
# framebuffer freed while Tailscale runs: TailnetNetPhase.h). MicroLink's changes
# live on the crossink branch of the Fledgewing/microlink fork (the submodule).
# A patch that no longer applies stops here with an error: that is the one
# place an upstream update can need overlay work.
set -euo pipefail

overlay="$(cd "$(dirname "$0")/.." && pwd)"
target="${1:?usage: apply.sh /path/to/CrossInk-Bookorbit}"
target="$(cd "$target" && pwd)"

[ -f "$target/platformio.ini" ] || { echo "not a CrossInk checkout: $target" >&2; exit 1; }
grep -q '^extra_configs = platformio.local.ini' "$target/platformio.ini" \
  || { echo "upstream no longer loads platformio.local.ini; the overlay needs updating" >&2; exit 1; }
[ -f "$overlay/third_party/microlink/components/microlink/include/microlink.h" ] \
  || git -C "$overlay" submodule update --init --recursive

# The overlay appends to ${base.extra_scripts}; an env with its own list would
# silently lose either upstream's scripts or ours.
for env in x4-pro x4-classic default; do
  if awk -v sec="[env:$env]" '$0==sec{f=1;next} /^\[/{f=0} f&&/^extra_scripts/{found=1} END{exit !found}' \
      "$target/platformio.ini"; then
    echo "upstream [env:$env] now sets its own extra_scripts; update overlay/platformio.local.ini" >&2
    exit 1
  fi
done

if [ -e "$target/platformio.local.ini" ] && ! cmp -s "$target/platformio.local.ini" "$overlay/overlay/platformio.local.ini"; then
  cp "$target/platformio.local.ini" "$target/platformio.local.ini.bak"
  echo "kept your previous platformio.local.ini as platformio.local.ini.bak"
fi
cp "$overlay/overlay/platformio.local.ini" "$target/platformio.local.ini"
ln -sfn "$overlay/tailnet" "$target/tailnet"
ln -sfn "$overlay/tailnet/ui" "$target/src/tailnet"
# Keep the symlink out of `git status` (upstream's build marks dirty trees)
# without editing upstream's .gitignore.
exclude="$(git -C "$target" rev-parse --git-path info/exclude)"
case "$exclude" in /*) ;; *) exclude="$target/$exclude" ;; esac
mkdir -p "$(dirname "$exclude")"
for p in /tailnet /src/tailnet; do
  grep -qx "$p" "$exclude" 2>/dev/null || echo "$p" >> "$exclude"
done

# One patch series per upstream line: overlay/patches/v2.0/ for CrossInk 2.x,
# overlay/patches/v1.6/ for everything older (until it exists, the 1.6 series is
# the top-level overlay/patches/*.patch). The version comes from the checkout's
# [crossink] version (what its build reports), else its tag.
version="$(awk '/^\[crossink\]/{f=1;next} /^\[/{f=0} f&&/^version *=/{sub(/^version *= */,"");print;exit}' \
  "$target/platformio.ini")"
[ -n "$version" ] || version="$(git -C "$target" describe --tags --abbrev=0 2>/dev/null || true)"
version="${version#v}"
major="${version%%.*}"
case "$major" in
  ''|*[!0-9]*) echo "cannot tell the CrossInk version of $target (got '$version')" >&2; exit 1 ;;
esac
if [ "$major" -ge 2 ]; then
  series="$overlay/overlay/patches/v2.0"
elif ls "$overlay"/overlay/patches/v1.6/*.patch >/dev/null 2>&1; then
  series="$overlay/overlay/patches/v1.6"
else
  series="$overlay/overlay/patches"
fi
ls "$series"/*.patch >/dev/null 2>&1 || { echo "no patches for CrossInk $version in $series" >&2; exit 1; }
echo "CrossInk $version: patches from ${series#"$overlay"/}"

for patch in "$series"/*.patch; do
  [ -e "$patch" ] || continue
  if git -C "$target" apply --reverse --check "$patch" 2>/dev/null; then
    continue  # already applied
  fi
  if ! git -C "$target" apply --3way "$patch"; then
    echo "patch $(basename "$patch") no longer applies to this CrossInk version; refresh it" >&2
    exit 1
  fi
done
echo "overlay applied to $target"
