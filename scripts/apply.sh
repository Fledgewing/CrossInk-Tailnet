#!/usr/bin/env bash
# Layer the Tailnet overlay onto a CrossInk-Bookorbit checkout.
#   scripts/apply.sh /path/to/CrossInk-Bookorbit
# Idempotent. Touches only git-ignored paths in the checkout:
#   platformio.local.ini  (copied from overlay/)
#   tailnet               (symlink to this repo's tailnet/, excluded via .git/info/exclude)
#   src/tailnet           (symlink to tailnet/ui, built with CrossInk's own sources)
# and applies overlay/patches/*.patch (today: two lines in src/main.cpp).
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

for patch in "$overlay"/overlay/patches/*.patch; do
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
