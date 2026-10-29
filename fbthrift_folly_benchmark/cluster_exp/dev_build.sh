#!/usr/bin/env bash
# Build and test the local working trees on suzhou950 without committing or
# pushing. Run locally: rsyncs folly-ca and fbthrift-ca to
# ~/cluster_aware/dev/{folly,fbthrift}, then runs build.sh from the dev tree
# once per argument against the dev install prefix (ins-dev, build dirs *-dev).
# Stops at the first failing step. Experiment builds come from sync.sh only.
#
# Usage: dev_build.sh "<build.sh target> [args]" ...
#   e.g. dev_build.sh folly "ca_test --gtest_filter='ClusterAware2*'"
# Env: HOST (suzhou950), ROOT, FOLLY_DIR (folly-ca), FBTHRIFT_DIR (fbthrift-ca);
#      the two dirs are relative to ROOT.
set -euo pipefail

ROOT=${ROOT:-/home/fanzhenhao/fbthrift+folly}
HOST=${HOST:-suzhou950}
FOLLY_DIR=${FOLLY_DIR:-folly-ca}
FBTHRIFT_DIR=${FBTHRIFT_DIR:-fbthrift-ca}
DEV=cluster_aware/dev

if [ $# -eq 0 ]; then
  echo "usage: $0 \"<build.sh target> [args]\" ..." >&2
  exit 2
fi

# Compare by checksum and do not keep source mtimes: only changed files are
# copied, and they get a fresh mtime, so ninja rebuilds whatever includes them
# (a kept, older mtime would leave objects built before the change looking
# up to date).
sync_tree() {
  rsync -rlpD --checksum --delete --exclude=.git --exclude=.superpowers \
    --exclude=gen-cpp2 \
    --exclude=__pycache__ --exclude=cluster_exp/results \
    "$ROOT/$1/" "$HOST:$DEV/$2/"
}

ssh "$HOST" mkdir -p "$DEV"
sync_tree "$FOLLY_DIR" folly
sync_tree "$FBTHRIFT_DIR" fbthrift

for step in "$@"; do
  echo "== build.sh $step"
  # shellcheck disable=SC2029
  ssh "$HOST" "export FOLLY_SRC=\$HOME/$DEV/folly FBTHRIFT_SRC=\$HOME/$DEV/fbthrift \
INS=\$HOME/cluster_aware/ins-dev SUFFIX=-dev && \
\$HOME/$DEV/fbthrift/fbthrift_folly_benchmark/cluster_exp/build.sh $step"
done
