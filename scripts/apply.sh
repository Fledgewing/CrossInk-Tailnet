#!/usr/bin/env bash
# Layer the Tailnet overlay onto a CrossInk-Bookorbit checkout.
#   scripts/apply.sh [--series v1.6|v2.0] /path/to/CrossInk-Bookorbit
# The patch series follows the checkout's CrossInk version (1.6.x -> overlay/patches/v1.6,
# 2.x -> overlay/patches/v2.0); --series (or CROSSINK_PATCH_SERIES) overrides it.
# Idempotent. Touches only git-ignored paths in the checkout:
#   platformio.local.ini  (copied from overlay/)
#   tailnet               (symlink to this repo's tailnet/, excluded via .git/info/exclude)
#   src/tailnet           (symlink to tailnet/ui, built with CrossInk's own sources)
# and applies the patch series for its CrossInk version (the Settings row; on the C3, the
# framebuffer freed while Tailscale runs: TailnetNetPhase.h). MicroLink's changes
# live on the crossink branch of the Fledgewing/microlink fork (the submodule).
# A patch that no longer applies stops here with an error: that is the one
# place an upstream update can need overlay work.
set -euo pipefail

overlay="$(cd "$(dirname "$0")/.." && pwd)"
series_override="${CROSSINK_PATCH_SERIES:-}"
if [ "${1:-}" = "--series" ]; then
  series_override="${2:?usage: apply.sh [--series v1.6|v2.0] /path/to/CrossInk-Bookorbit}"
  shift 2
fi
target="${1:?usage: apply.sh [--series v1.6|v2.0] /path/to/CrossInk-Bookorbit}"
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

# One patch series per upstream line: overlay/patches/v1.6/ for CrossInk 1.6.x,
# overlay/patches/v2.0/ for 2.x. The version comes from the checkout's
# [crossink] version (what its build reports), else its tag.
version="$(awk '/^\[crossink\]/{f=1;next} /^\[/{f=0} f&&/^version *=/{sub(/^version *= */,"");print;exit}' \
  "$target/platformio.ini")"
[ -n "$version" ] || version="$(git -C "$target" describe --tags --abbrev=0 2>/dev/null || true)"
version="${version#v}"
if [ -n "$series_override" ]; then
  name="${series_override#overlay/patches/}"
  echo "series $name chosen by override (checkout reports '${version:-unknown}')"
else
  case "$version" in
    1.6.*) name=v1.6 ;;
    2.*) name=v2.0 ;;
    *) echo "no patch series for CrossInk '${version:-unknown}' in $target (known: 1.6.x -> v1.6, 2.x -> v2.0);" \
         "pass --series v1.6|v2.0 to choose one" >&2
       exit 1 ;;
  esac
fi
series="$overlay/overlay/patches/$name"
[ -d "$series" ] || { echo "no patch series '$name' (expected one of: $(cd "$overlay/overlay/patches" && echo v*))" >&2; exit 1; }
ls "$series"/*.patch >/dev/null 2>&1 || { echo "no patches for CrossInk $version in $series" >&2; exit 1; }
echo "CrossInk $version: patches from ${series#"$overlay"/}"

# A rerun on a checkout this series was already applied to is a no-op. Checking each patch in
# reverse is not enough: a later patch can change the lines an earlier one added.
stamp="$(git -C "$target" rev-parse --git-path tailnet-overlay-applied)"
case "$stamp" in /*) ;; *) stamp="$target/$stamp" ;; esac
want="$(git -C "$target" rev-parse HEAD) $(cat "$series"/*.patch | git hash-object --stdin)"
if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$want" ] && ! git -C "$target" diff --quiet HEAD; then
  echo "overlay already applied to $target"
  exit 0
fi

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
echo "$want" > "$stamp"
echo "overlay applied to $target"
