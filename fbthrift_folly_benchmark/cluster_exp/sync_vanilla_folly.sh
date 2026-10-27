#!/usr/bin/env bash
# Sync a folly tree identical to folly-ca HEAD but with the EventBase
# loop-callback wakeup fix reverted to suzhou950:~/cluster_aware/src/folly-vanilla,
# for the vanilla-vs-fixed comparison. Run locally.
set -euo pipefail

ROOT=${ROOT:-/home/fanzhenhao/fbthrift+folly}
HOST=${HOST:-suzhou950}
FIX=${FIX:-c30de36b7} # "io/async: run loop callbacks queued by internal handlers before blocking"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git -C "$ROOT/folly-ca" archive HEAD | tar -x -C "$tmp"
for f in folly/io/async/EventBase.cpp folly/io/async/EventBase.h; do
  git -C "$ROOT/folly-ca" show "$FIX^:$f" > "$tmp/$f"
done
rsync -a --delete "$tmp/" "$HOST:cluster_aware/src/folly-vanilla/"
echo "synced folly-vanilla (HEAD $(git -C "$ROOT/folly-ca" rev-parse --short HEAD) minus $FIX)"
