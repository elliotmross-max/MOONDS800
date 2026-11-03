#!/usr/bin/env bash
# vendor-header.sh -- keep vendor/schwung/plugin_api_v1.h in step with upstream.
#
# The header is VENDORED: committed into this repo rather than fetched at build
# time. That is what Model-72 and setBFree do, and it is what makes the repo
# buildable by someone who clones it. But a vendored header drifts, and a stale
# one is how a module ends up compiled against an ABI that no longer exists --
# which is exactly the failure that boot-looped a device via breakbeat's header
# drift.
#
# So: this script fetches upstream and tells you whether your copy matches.
#
#   bash scripts/vendor-header.sh check     (default) compare, exit 1 on drift
#   bash scripts/vendor-header.sh update    overwrite the vendored copy
#
# Requires curl. Upstream is charlesvestal/schwung, MIT licensed.

set -euo pipefail
cd "$(dirname "$0")/.."

LOCAL="vendor/schwung/plugin_api_v1.h"
URL="https://raw.githubusercontent.com/charlesvestal/schwung/main/src/host/plugin_api_v1.h"
MODE="${1:-check}"

if [ ! -f "$LOCAL" ]; then
  echo "no $LOCAL -- run: bash scripts/vendor-header.sh update" >&2
  exit 1
fi

TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT

if ! curl -fsSL -o "$TMP" "$URL"; then
  echo "could not reach upstream. Check your network, or the URL if the file moved." >&2
  echo "  $URL" >&2
  exit 2
fi

case "$MODE" in
  update)
    cp "$TMP" "$LOCAL"
    echo "updated $LOCAL ($(wc -l < "$LOCAL") lines)"
    ;;
  check)
    if diff -q "$LOCAL" "$TMP" > /dev/null; then
      echo "ok: $LOCAL matches upstream ($(wc -l < "$LOCAL") lines)"
    else
      echo "DRIFT: $LOCAL differs from upstream." >&2
      echo >&2
      diff -u "$LOCAL" "$TMP" | head -60 >&2
      echo >&2
      echo "Review the diff above. If upstream only added fields at the end," >&2
      echo "run 'bash scripts/vendor-header.sh update' and rebuild." >&2
      echo "If field ORDER changed, the wrapper needs its _Static_asserts" >&2
      echo "re-checking -- a reordered vtable calls the wrong function quietly." >&2
      exit 1
    fi
    ;;
  *)
    echo "usage: bash scripts/vendor-header.sh [check|update]" >&2
    exit 2
    ;;
esac
