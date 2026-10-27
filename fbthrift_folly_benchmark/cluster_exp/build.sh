#!/usr/bin/env bash
# Build the baseline dependency chain, the press benchmark and the
# cluster-aware folly tests on suzhou950.
#
# Usage: build.sh folly|fizz|wangle|fbthrift|bench|all
#        build.sh ca_test|evb_test [gtest args...]
# Env overrides (e.g. for the vanilla-EventBase comparison build):
#   INS=<install prefix>  FOLLY_SRC=<folly source>  SUFFIX=<build dir suffix>
set -euo pipefail

CA=${CA:-$HOME/cluster_aware}
SRC=$CA/src
BLD=$CA/build
INS=${INS:-$CA/ins}
FOLLY_SRC=${FOLLY_SRC:-$SRC/folly}
SUFFIX=${SUFFIX:-}
JOBS=${JOBS:-128}

COMMON=(-G Ninja -DCMAKE_BUILD_TYPE=Release
  "-DCMAKE_INSTALL_PREFIX=$INS" "-DCMAKE_PREFIX_PATH=$INS"
  -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  # openEuler leaves this empty and folly then assumes x86_64 (adds -mavx etc.).
  "-DCMAKE_LIBRARY_ARCHITECTURE=$(uname -m)-linux-gnu")

# Always re-run configure: folly collects sources by glob, so new files are
# only picked up by a fresh configure.
lib() {
  local name=$1 src=$2
  shift 2
  cmake -S "$src" -B "$BLD/$name$SUFFIX" "${COMMON[@]}" "$@"
  cmake --build "$BLD/$name$SUFFIX" --target install -j "$JOBS"
}

bench() {
  local dir=$SRC/fbthrift/fbthrift_folly_benchmark
  (cd "$dir" && "$INS/bin/thrift1" --gen mstch_cpp2 -o . press.thrift)
  cmake -S "$dir" -B "$BLD/bench$SUFFIX" "${COMMON[@]}"
  cmake --build "$BLD/bench$SUFFIX" -j "$JOBS"
}

evb_test() {
  cmake -S "$FOLLY_SRC/folly/io/async/test/evb_wakeup" -B "$BLD/evb_test$SUFFIX" "${COMMON[@]}"
  cmake --build "$BLD/evb_test$SUFFIX" -j "$JOBS"
  (cd "$BLD/evb_test$SUFFIX" && ./evb_wakeup_test "$@")
}

ca_test() {
  cmake -S "$FOLLY_SRC/folly/executors/test/cluster_aware" -B "$BLD/ca_test$SUFFIX" "${COMMON[@]}"
  cmake --build "$BLD/ca_test$SUFFIX" -j "$JOBS"
  (cd "$BLD/ca_test$SUFFIX" && ./cluster_aware_test "$@")
}

target=${1:-}
[ $# -gt 0 ] && shift
case $target in
  folly) lib folly "$FOLLY_SRC" -DBUILD_TESTS=OFF ;;
  fizz) lib fizz "$SRC/fizz/fizz" -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF ;;
  wangle) lib wangle "$SRC/wangle/wangle" -DBUILD_TESTS=OFF ;;
  fbthrift) lib fbthrift "$SRC/fbthrift" -Denable_tests=OFF ;;
  bench) bench ;;
  ca_test) ca_test "$@" ;;
  evb_test) evb_test "$@" ;;
  all)
    for t in folly fizz wangle fbthrift; do "$0" "$t"; done
    bench
    ;;
  *)
    echo "usage: $0 folly|fizz|wangle|fbthrift|bench|all|ca_test|evb_test [gtest args]" >&2
    exit 2
    ;;
esac
