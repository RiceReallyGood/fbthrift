#!/usr/bin/env bash
# Sync local sources to suzhou950:~/cluster_aware/src and record source versions.
# Run locally (suzhou950 cannot reach GitHub).
set -euo pipefail

ROOT=${ROOT:-/home/fanzhenhao/fbthrift+folly}
HOST=${HOST:-suzhou950}
REMOTE=cluster_aware

ssh "$HOST" "mkdir -p ~/$REMOTE/src"

RS=(rsync -a --delete --exclude .git)
# Keep the repos' own build/ dirs (build/fbcode_builder/CMake is needed);
# only skip the benchmark's local build output and generated code.
"${RS[@]}" "$ROOT/folly-ca/" "$HOST:$REMOTE/src/folly/"
"${RS[@]}" --exclude /fbthrift_folly_benchmark/build/ \
  --exclude /fbthrift_folly_benchmark/gen-cpp2/ \
  "$ROOT/fbthrift-ca/" "$HOST:$REMOTE/src/fbthrift/"
"${RS[@]}" "$ROOT/third_party_src/fizz/" "$HOST:$REMOTE/src/fizz/"
"${RS[@]}" "$ROOT/third_party_src/wangle/" "$HOST:$REMOTE/src/wangle/"

{
  for repo in folly-ca fbthrift-ca third_party_src/fizz third_party_src/wangle; do
    dirty=$(git -C "$ROOT/$repo" status --short | wc -l)
    echo "$repo $(git -C "$ROOT/$repo" rev-parse HEAD) dirty_files=$dirty"
  done
} | ssh "$HOST" "cat > ~/$REMOTE/SOURCE_VERSIONS"

echo "synced to $HOST:~/$REMOTE/src"
